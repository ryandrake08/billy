# Billy — a Big Mouth Billy Bass LLM voice assistant

Converting a Big Mouth Billy Bass novelty toy into an **offline, LAN-only** voice assistant:
an ESP32-S3 in the fish captures speech and drives the motors, while a backend box does the
heavy lifting (speech-to-text → LLM → text-to-speech).

The fish is a **thin client**. All intelligence lives on the backend: a persona/application
shim in front of llama.cpp, with STT and TTS as dumb audio↔text engines. The fish just
captures audio, conducts three HTTP calls, plays the reply, and animates the mouth.

## Layout

| Path | What |
|---|---|
| `client/` | Thin CLI reference client — the fish↔backend protocol the ESP32 reimplements. |
| `firmware/` | The ESP32-S3 firmware controlling the motors, speaker, and microphone. See `firmware/README.md`. |
| `shim/` | The backend application-logic/persona shim (FastAPI, in front of llama.cpp). |
| `board/` | Custom PCB design (KiCad). |

## The backend services

| Service | Port | Role |
|---|---|---|
| shim (`billy-shim`) | 8000 | Persona, session history, text cleaning, sentence streaming. The fish's only brain hop. |
| llama.cpp | 8080 | LLM (Qwen3-8B Q4, non-thinking). Behind the shim. |
| whisper.cpp | 8081 | STT (`/inference`). Called directly by the client. |
| Kokoro | 8880 | TTS (voice `am_onyx`). Called directly by the client. |

All four run under systemd (`Restart=always`, enabled at boot).