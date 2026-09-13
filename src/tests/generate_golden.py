#!/usr/bin/env python3
"""Regenerate the golden STT input WAVs in golden/. Run by hand only when a translation or voice
choice changes; test_language_integration.py also calls this itself (ensure_golden()) whenever a
golden/<lang>.wav is missing, since these are gitignored rather than committed -- edge-tts is an
unofficial, reverse-engineered API with no clear license to redistribute its output audio, so each
machine generates its own copies instead (a one-time, ~1 minute step needing internet; the test
itself needs no internet beyond that). Uses Microsoft Edge's free neural TTS (edge-tts), not
Kokoro: the point of these fixtures is high-quality, real-world-like pronunciation per language,
since Kokoro's own non-English output has been observed to sometimes confuse whisper's
transcription on a synthetic TTS->STT round trip.

Every phrase is the same instruction translated per language -- "Repeat back to me: I am Billy,
a fish on the wall, and I'm here to tell jokes." -- so the integration test can ask for something
whisper.cpp/Qwen can comply with almost verbatim, cutting down on how much of the LLM's normal
free-form variance the test has to tolerate.

Run:  ./.venv/bin/python generate_golden.py   (after: python3 -m venv .venv && .venv/bin/pip install -r requirements.txt)
Requires ffmpeg on PATH (converts edge-tts's mp3 output to 16 kHz mono PCM16 WAV, matching what
whisper.cpp actually receives from the reference clients: src/client/billy_cli.py's SAMPLE_RATE).
"""
import asyncio
import pathlib
import subprocess

OUT_DIR = pathlib.Path(__file__).parent / "golden"

# language key (matches text.py's LANGUAGE_VOICES / whisper's detected_language) ->
# (edge-tts voice, phrase). One human-authored translation per language, not LLM-generated --
# reused verbatim every time these fixtures are regenerated so they stay stable across runs.
PHRASES = {
    "english": ("en-US-GuyNeural",
                "Repeat back to me: I am Billy, a fish on the wall, and I'm here to tell jokes."),
    "spanish": ("es-ES-AlvaroNeural",
                "Repite esto: Soy Billy, un pez en la pared, y estoy aqui para contar chistes."),
    "portuguese": ("pt-BR-AntonioNeural",
                   "Repita isto para mim: Eu sou o Billy, um peixe na parede, "
                   "e estou aqui para contar piadas."),
    "chinese": ("zh-CN-YunjianNeural",
                "跟我重复一遍:我是比利,一条挂在墙上的鱼,我是来讲笑话的。"),
    "japanese": ("ja-JP-KeitaNeural",
                 "私の後に繰り返してください。僕はビリー、壁にかかっている魚で、"
                 "ジョークを言いに来たんだ。"),
    "hindi": ("hi-IN-MadhurNeural",
              "मेरे पीछे दोहराओ: मैं बिली हूँ, दीवार पर लगी एक मछली, "
              "और मैं यहाँ चुटकुले सुनाने आया हूँ।"),
    "italian": ("it-IT-DiegoNeural",
                "Ripeti dopo di me: Sono Billy, un pesce appeso al muro, "
                "e sono qui per raccontare barzellette."),
}


async def synth(voice, text, mp3_path):
    import edge_tts
    communicate = edge_tts.Communicate(text, voice)
    with open(mp3_path, "wb") as f:
        async for chunk in communicate.stream():
            if chunk["type"] == "audio":
                f.write(chunk["data"])


def main():
    OUT_DIR.mkdir(exist_ok=True)
    for lang, (voice, text) in PHRASES.items():
        mp3_path = OUT_DIR / f"{lang}.mp3"
        wav_path = OUT_DIR / f"{lang}.wav"
        print(f"{lang} ({voice}) ...", end=" ", flush=True)
        asyncio.run(synth(voice, text, mp3_path))
        subprocess.run(
            ["ffmpeg", "-y", "-loglevel", "error", "-i", str(mp3_path),
             "-ar", "16000", "-ac", "1", "-sample_fmt", "s16", str(wav_path)],
            check=True,
        )
        mp3_path.unlink()
        print(f"-> {wav_path}")


if __name__ == "__main__":
    main()
