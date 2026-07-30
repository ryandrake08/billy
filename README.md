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
- ✅ **Stage 2 — ESP32 dev system: complete.** WiFi + backend reachability verified on
  hardware; one-command flash works; step-debug/breakpoints verified over the S3's native
  USB-Serial-JTAG.
- ✅ **Stage 3 — ESP32 audio + I/O on the bench: complete.** Audio self-test, STT capture,
  activation (button + mode switch, including a manual button override while in wake-word mode),
  and wake-word detection all verified on the bench. Motors are wired and bench-verified —
  duty swept on all three, and tail-flap / head-raise-relax / mouth-open-close choreography
  confirmed working end-to-end in the full backend round trip. Button-mode deep sleep is
  implemented and bench-verified (a button press wakes the chip via a full reboot straight into
  LISTEN). The two earlier open findings (head deflection, buck brownout under motor load) are
  resolved — both root-caused to spent-battery sag, not a firmware/board issue. The bare-module
  standby current measurement is waived for now (needs hardware that doesn't exist yet); deep
  sleep itself is verified functionally. See `WIRING.md` for detail.
- 🔶 **Stage 4 — Fish integration: in progress.** Motor wiring and choreography done on the bench
  using the toy's own three motors (mouth/head/tail), not yet mounted in the reassembled housing.
  Photocell wired and read at boot, no consumer yet by design. Speaker and TTS voice both stay
  stock — an upgraded driver was tried and reverted. Packaging skips the soldered-protoboard
  stage in favor of a custom PCB (design underway in `board/`); Micro-USB flashing, no OTA, and
  the power-input mux part are decided. See `IMPLEMENTATION_PLAN.md` for the open items.

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