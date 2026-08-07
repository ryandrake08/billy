#!/usr/bin/env python3
"""
Billy Bass — CLI reference client (thin fish client).

Exercises the full backend voice pipeline from any host with a mic + speaker, so the
STT -> brain -> TTS chain is validated end-to-end before any ESP32 exists. This client
*is* the documented fish<->backend protocol contract that the ESP32 firmware reimplements.

The fish is a THIN client: it holds no persona, no conversation history, and does no text
cleaning. All of that lives in the backend shim, which keeps the fish model-agnostic and
lets the character change with no reflash. The fish just conducts three calls per turn,
two of them straight to the audio engines:

  STT   : POST {stt}/inference       multipart file=<wav 16k mono>, response_format=json -> {"text": ...}
  brain : POST {shim}/v1/respond     JSON {session, text}; SSE -> {"sentence", "voice"} per line, then [DONE]
  TTS   : POST {tts}/v1/audio/speech JSON {model,input,voice,response_format=wav}          -> wav bytes

The shim already returns clean, spoken-ready sentences (persona applied, <think> stripped,
markdown/emoji scrubbed, split on sentence boundaries), each carrying the voice it should be
spoken in. This CLI is a dev tool rather than the fish itself, so it adds one liberty the fish
doesn't have: --voice, if given, overrides the shim's choice for every sentence (handy for A/B
listening). Priority is --voice > the shim's per-sentence voice > a built-in default.

Turn pipeline: record -> STT -> shim (streams sentences) -> TTS per sentence -> play, with
sentence 1 spoken while later sentences are still being generated/synthesized (a synth thread
runs ahead of the playback loop). Prints per-turn latency (end-of-speech -> first audio) to
validate the ~2 s target.

Usage:
  pip install -r requirements.txt
  ./billy_cli.py --host <backend-host>     # ports: shim :8000, STT :8081, TTS :8880
"""
import argparse
import io
import json
import queue
import sys
import threading
import time

import httpx
import numpy as np
import sounddevice as sd
import soundfile as sf
import soxr

SAMPLE_RATE = 16000  # whisper wants 16 kHz mono
DEFAULT_VOICE = "am_onyx"  # used only if the shim omits "voice" and --voice wasn't given

_DEVICE_SR = None


def _device_sr():
    global _DEVICE_SR
    if _DEVICE_SR is None:
        _DEVICE_SR = int(sd.query_devices(kind="output")["default_samplerate"])
    return _DEVICE_SR


def play_audio(audio, sr):
    """Play float32 audio, resampling to the output device's native rate first.
    PortAudio's driver-side resampling of a mismatched rate (e.g. 24 kHz TTS on a 48 kHz
    device) produced static; resampling in Python with soxr avoids it."""
    dev = _device_sr()
    if sr != dev:
        audio = soxr.resample(audio, sr, dev)
    sd.play(audio, dev)
    sd.wait()


def record_utterance():
    """Press Enter to start, speak, press Enter to stop. Returns wav bytes (16k mono int16), or None."""
    input("\n[Enter] to start talking… ")
    frames = []

    def cb(indata, _frames, _time, status):
        if status:
            print(status, file=sys.stderr)
        frames.append(indata.copy())

    stream = sd.InputStream(samplerate=SAMPLE_RATE, channels=1, dtype="int16", callback=cb)
    stream.start()
    input("🎤 recording — [Enter] to stop… ")
    stream.stop()
    stream.close()
    if not frames:
        return None
    audio = np.concatenate(frames, axis=0)
    buf = io.BytesIO()
    sf.write(buf, audio, SAMPLE_RATE, format="WAV", subtype="PCM_16")
    return buf.getvalue()


def transcribe(client, stt_url, wav_bytes):
    """whisper.cpp /inference (multipart) -> transcript text. Called directly (STT is a dumb
    audio->text transform; only the brain hop goes through the shim)."""
    files = {"file": ("rec.wav", wav_bytes, "audio/wav")}
    data = {"response_format": "json", "temperature": "0.0"}
    r = client.post(f"{stt_url}/inference", files=files, data=data, timeout=60)
    r.raise_for_status()
    return r.json().get("text", "").strip()


def stream_billy(client, shim_url, session, text):
    """POST the utterance to the shim and yield each (sentence, voice) as it streams back.
    The shim owns the persona, history, and text cleaning — sentences arrive ready to speak,
    each tagged with the voice it should be spoken in (voice is None if the shim omits it)."""
    body = {"session": session, "text": text}
    with client.stream("POST", f"{shim_url}/v1/respond", json=body, timeout=120) as r:
        r.raise_for_status()
        for line in r.iter_lines():
            if not line.startswith("data:"):
                continue
            payload = line[len("data:"):].strip()
            if payload == "[DONE]":
                break
            try:
                obj = json.loads(payload)
            except json.JSONDecodeError:
                continue
            if "sentence" in obj:
                yield obj["sentence"], obj.get("voice")
            elif "error" in obj:
                print(f"  [shim error: {obj['error']}]", file=sys.stderr)


def reset_session(client, shim_url, session):
    """Start each CLI run with a fresh conversation (the shim keeps history server-side, so
    without this a new run would inherit the previous run's context)."""
    try:
        client.post(f"{shim_url}/v1/reset", json={"session": session}, timeout=10)
    except httpx.HTTPError as e:  # noqa: BLE001 — non-fatal; stale history just lingers
        print(f"  [reset failed: {e}]", file=sys.stderr)


def tts_synth(client, tts_url, voice, text):
    """Kokoro /v1/audio/speech -> (float32 audio, samplerate). Called directly (TTS is a dumb
    text->audio transform)."""
    body = {"model": "kokoro", "input": text, "voice": voice, "response_format": "wav"}
    r = client.post(f"{tts_url}/v1/audio/speech", json=body, timeout=120)
    r.raise_for_status()
    audio, sr = sf.read(io.BytesIO(r.content), dtype="float32")
    return audio, sr


def speak_turn(client, tts_url, voice_override, sentence_iter, on_first_audio):
    """Synthesize sentences on a worker thread (running ahead) while the main thread plays them
    in order. Voice priority: voice_override (--voice, if given) > the shim's per-sentence
    choice > DEFAULT_VOICE."""
    audio_q = queue.Queue()
    DONE = object()

    def synth_worker():
        for sent, voice in sentence_iter:
            try:
                chosen_voice = voice_override or voice or DEFAULT_VOICE
                audio, sr = tts_synth(client, tts_url, chosen_voice, sent)
                audio_q.put((sent, audio, sr))
            except Exception as e:  # noqa: BLE001 — reference client, keep going
                print(f"  [tts error: {e}]", file=sys.stderr)
        audio_q.put(DONE)

    worker = threading.Thread(target=synth_worker, daemon=True)
    worker.start()

    first = True
    while True:
        item = audio_q.get()
        if item is DONE:
            break
        sent, audio, sr = item
        if first:
            on_first_audio()
            first = False
        print(f"  🐟 {sent}")
        play_audio(audio, sr)
    worker.join()


def main():
    ap = argparse.ArgumentParser(description="Billy Bass CLI reference client")
    ap.add_argument("--host", default=None,
                    help="backend host (hostname or IP); builds the default shim/STT/TTS URLs")
    ap.add_argument("--shim-url", default=None, help="override, e.g. http://<backend-host>:8000")
    ap.add_argument("--stt-url", default=None)
    ap.add_argument("--tts-url", default=None)
    ap.add_argument("--voice", default=None,
                    help="override the shim's per-sentence voice choice for the whole session "
                         "(Kokoro: see /web UI for the list); omit to use whatever the shim sends")
    ap.add_argument("--session", default="default", help="shim conversation id")
    args = ap.parse_args()

    shim_url = args.shim_url or (f"http://{args.host}:8000" if args.host else None)
    stt_url = args.stt_url or (f"http://{args.host}:8081" if args.host else None)
    tts_url = args.tts_url or (f"http://{args.host}:8880" if args.host else None)
    if not (shim_url and stt_url and tts_url):
        ap.error("provide --host (backend hostname/IP), or override each of "
                 "--shim-url/--stt-url/--tts-url")

    voice_note = f"voice override {args.voice}" if args.voice else f"shim-chosen voice (fallback {DEFAULT_VOICE})"
    print(f"Billy CLI — shim {shim_url} · STT {stt_url} · TTS {tts_url} · {voice_note}")
    print("Ctrl-C to quit.")

    client = httpx.Client()
    reset_session(client, shim_url, args.session)
    try:
        while True:
            wav = record_utterance()
            if not wav:
                continue
            t_end = time.time()

            text = transcribe(client, stt_url, wav)
            t_stt = time.time()
            if not text:
                print("  (heard nothing)")
                continue
            print(f"  🗣  {text}")

            first_audio = {}
            sentences = stream_billy(client, shim_url, args.session, text)
            speak_turn(client, tts_url, args.voice, sentences,
                       lambda: first_audio.setdefault("t", time.time()))

            t_first = first_audio.get("t", time.time())
            print(f"  ⏱  STT {t_stt - t_end:.2f}s · end→first-audio {t_first - t_end:.2f}s")
    except KeyboardInterrupt:
        print("\nbye 🐟")
    finally:
        client.close()


if __name__ == "__main__":
    main()
