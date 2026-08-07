# Billy CLI — thin reference client

A command-line stand-in for the fish: it drives the full backend voice pipeline
(record → STT → shim → TTS → play) from any host with a mic + speaker, and it *is* the
documented fish↔backend protocol the ESP32 firmware reimplements.

It is a **thin client** by design: it holds no persona, no conversation history, and does no
text cleaning. All of that lives in the backend shim, so the fish stays model-agnostic and the
character changes with no reflash. The CLI just conducts three calls per turn — two straight
to the audio engines, one to the shim's brain endpoint.

## Run

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt
./billy_cli.py --host <backend-host>    # shim :8000, STT :8081, TTS :8880
```

Press **Enter** to start talking, speak, press **Enter** to stop. `Ctrl-C` quits.

Options: `--voice <kokoro-voice>` (browse the list at `http://<backend-host>:8880/web`) overrides
the voice for every sentence in the session — otherwise each sentence uses the shim's own choice,
falling back to `am_onyx` if the shim omits one. `--session <id>` sets the shim conversation id;
`--shim-url/--stt-url/--tts-url` override individual endpoints.

## What it exercises (the protocol contract)

| Call | Request |
|---|---|
| STT | `POST {stt}/inference` — multipart `file=<wav 16k mono>`, `response_format=json` → `{"text": …}` |
| brain | `POST {shim}/v1/respond` — `{session, text}`, SSE → `{"sentence": …, "voice": …}` per line, then `[DONE]` |
| TTS | `POST {tts}/v1/audio/speech` — `{model, input, voice, response_format:"wav"}` → wav bytes |

The shim streams **clean, spoken-ready sentences** (persona applied, `<think>` stripped,
markdown/emoji scrubbed, split on sentence boundaries), each tagged with the voice it should be
spoken in; the CLI synthesizes each verbatim and speaks it as it arrives (a synth thread runs
ahead of playback), so speech starts on sentence 1 while the rest is still being generated. Each
turn prints STT time and **end-of-speech → first-audio** latency.

STT and TTS are called **directly** — they're dumb audio↔text transforms; only the brain hop
goes through the shim. The shim keeps history server-side, so the CLI resets its session on
startup to begin each run fresh.

## Notes

- Uses the system default audio devices. To pick a specific one, set it in your OS or via
  `sounddevice` (`python3 -c "import sounddevice as sd; print(sd.query_devices())"`).
- Playback resamples the 24 kHz TTS audio to the output device's native rate with `soxr`
  before playing.
- This host must reach the backend on ports 8000/8081/8880.
