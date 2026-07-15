# Billy CLI — thin reference client

A command-line stand-in for the fish: it drives the full homelab voice pipeline
(record → STT → shim → TTS → play) from any host with a mic + speaker. It validates the
chain end-to-end **before** any ESP32 exists, and it *is* the documented protocol the ESP32
firmware reimplements in Stage 3.

It is a **thin client** by design: it holds no persona, no conversation history, and does no
text cleaning. All of that lives in the homelab shim (`SCOPING.md` §8.3), so the fish stays
model-agnostic and the character changes with no reflash. The CLI just conducts three calls
per turn — two straight to the audio engines, one to the shim's brain endpoint.

## Run

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt
./billy_cli.py --host gpu-host          # shim :8000, STT :8081, TTS :8880
```

Press **Enter** to start talking, speak, press **Enter** to stop. `Ctrl-C` quits.

Options: `--voice <kokoro-voice>` (browse the list at `http://gpu-host:8880/web`),
`--session <id>` (shim conversation id), `--shim-url/--stt-url/--tts-url` to override
individual endpoints.

## What it exercises (the protocol contract)

| Call | Request |
|---|---|
| STT | `POST {stt}/inference` — multipart `file=<wav 16k mono>`, `response_format=json` → `{"text": …}` |
| brain | `POST {shim}/v1/respond` — `{session, text}`, SSE → `{"sentence": …}` per line, then `[DONE]` |
| TTS | `POST {tts}/v1/audio/speech` — `{model, input, voice, response_format:"wav"}` → wav bytes |

The shim streams **clean, spoken-ready sentences** (persona applied, `<think>` stripped,
markdown/emoji scrubbed, split on sentence boundaries); the CLI synthesizes each verbatim and
speaks it as it arrives (a synth thread runs ahead of playback), so speech starts on sentence 1
while the rest is still being generated. Each turn prints STT time and **end-of-speech →
first-audio** latency (the ~2 s target from `SCOPING.md` §2).

STT and TTS are called **directly** — they're dumb audio↔text transforms; only the brain hop
goes through the shim. The shim keeps history server-side, so the CLI resets its session on
startup to begin each run fresh.

## Notes

- Uses the system default audio devices. To pick a specific one, set it in your OS or via
  `sounddevice` (`python3 -c "import sounddevice as sd; print(sd.query_devices())"`).
- Playback resamples the 24 kHz TTS audio to the output device's native rate with `soxr`
  before playing — PortAudio's driver-side resampling of the mismatched rate produced static.
- This host must reach `gpu-host` on ports 8000/8081/8880.
- The voice `am_onyx` was picked from a Kokoro bake-off; the choice is recorded in `SCOPING.md` §2.2.
