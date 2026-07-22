# Billy — a Big Mouth Billy Bass LLM voice assistant

Converting a Big Mouth Billy Bass novelty toy into an **offline, LAN-only** voice assistant:
an ESP32-S3 in the fish captures speech and drives the motors, while a backend box does the
heavy lifting (speech-to-text → LLM → text-to-speech). No internet, ever — a hard requirement.

The fish is a **thin client**. All intelligence lives on the backend: a persona/application
shim in front of llama.cpp, with STT and TTS as dumb audio↔text engines. The fish just
captures audio, conducts three HTTP calls, plays the reply, and animates the mouth.

## Status

- ✅ **Stage 1 — Backend pipeline + reference client: complete.** Full spoken conversation
  works end-to-end via a CLI stand-in, at ~2 s first-audio, with the character voice locked.
- ⏭ **Stage 2 — ESP32 dev system:** next up, gated on the ESP32-S3 board arriving.
- ⬜ **Stage 3 — ESP32 audio + I/O on the bench**, ⬜ **Stage 4 — Fish integration.**

See `SCOPING.md` for all hardware/architecture decisions and the rationale behind the staged
build above.

## Layout

| Path | What |
|---|---|
| `client/` | The thin CLI reference client — the fish↔backend protocol the ESP32 reimplements. |
| `firmware/` | The actual ESP32 firmware controlling the various motors, speaker, microphone, etc. |
| `shim/` | The backend application-logic/persona shim (FastAPI, in front of llama.cpp). |

## The backend services

| Service | Port | Role |
|---|---|---|
| shim (`billy-shim`) | 8000 | Persona, session history, text cleaning, sentence streaming. The fish's only brain hop. |
| llama.cpp | 8080 | LLM (Qwen3-8B Q4, non-thinking). Behind the shim. |
| whisper.cpp | 8081 | STT (`/inference`). Called directly by the client. |
| Kokoro | 8880 | TTS (voice `am_onyx`). Called directly by the client. |

All four run under systemd (`Restart=always`, enabled at boot).