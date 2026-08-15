# Smart Mouth Billy Bass LLM voice assistant

Billy is a Big Mouth Billy Bass novelty toy converted into an **offline, LAN-only** voice
assistant. An ESP32-S3 inside the fish captures speech, drives the motors, and plays back
audio; a separate GPU box does speech-to-text, LLM inference, and text-to-speech. There is no
internet dependency anywhere in the pipeline.

The fish is a **thin client**: it holds no persona, no conversation history, and does no text
cleaning. All of that lives on the backend, behind a small application-logic "shim" in front of
the LLM. The fish just captures audio, makes three HTTP calls per turn, plays the reply, and
animates the mouth/head/tail in sync with it.

## Architecture

```
        FISH (ESP32-S3)                                BACKEND (LAN)
  ┌──────────────────────────┐                  ┌───────────────────────────┐
  │ mic → capture utterance  │───-─ STT ───────▶│ whisper.cpp        :8081  │
  │                          │◀─── text ────────│                           │
  |                          |                  |                           |    ┌─────────────────┐
  │ text → shim              │──── brain ──────▶│ shim (persona,     :8000  │──▶ | llama.cpp :8080 |
  │                          │◀── sentences ────│  history, text cleaning)  │    └─────────────────┘
  │                          │                  │                           │
  │ sentence → TTS           │──── TTS ────────▶│ Kokoro             :8880  │
  │                          │◀─── audio ───────│                           │
  │ amp + motors             │                  └───────────────────────────┘
  └──────────────────────────┘
```

Three calls per conversational turn:

| Call | Target | Request → Response |
|---|---|---|
| STT | whisper.cpp `:8081`, direct | `POST /inference` (multipart wav) → `{"text": ...}` |
| brain | shim `:8000` | `POST /v1/respond` `{session, text}` → SSE stream of `{"sentence", "voice"}` |
| TTS | Kokoro `:8880`, direct | `POST /v1/audio/speech` `{model, input, voice}` → wav bytes |

STT and TTS are dumb audio↔text transforms called **directly**; only the brain hop goes through
the shim, which is a text-only service that never touches audio. The shim streams sentences as
the LLM generates them, so the caller can synthesize and play sentence 1 while sentence 2 is
still being generated — this pipelining is what keeps first-audio latency low.

This exact contract is implemented three times, deliberately kept in sync: the reference clients
`src/client/billy_cli.py` (Python) and `src/cli/` (C), and the ESP32 firmware
(`src/firmware/main/net.c`). The CLIs exist to document the protocol the firmware has to match —
the C one doubles as a check on how much of the firmware's own protocol code (WAV header,
hand-rolled JSON parsing, SSE reassembly) is genuinely ESP-IDF-independent and portable to a
host build; it turned out to be all of it.

## Repository layout

| Path | What |
|---|---|
| `board/` | Custom PCB for the fish's electronics (KiCad). |
| `src/firmware/` | ESP32-S3 firmware (C, ESP-IDF). |
| `src/shim/` | Backend application-logic layer (Python/FastAPI). |
| `src/client/` | CLI reference client (Python) — a mic+speaker stand-in for the fish. |
| `src/cli/` | CLI reference client (C) — the same stand-in, built against libcurl/PortAudio/soxr. |

---

## `board/` — Custom PCB

KiCad project at `board/billy/`. Replaces the toy's stock control board with a single board
carrying the ESP32-S3, motor drivers, amp, buck regulator, and power-input protection —
designed to fit inside the original toy's chassis (~70×50mm).

| Part | Role |
|---|---|
| ESP32-S3-WROOM-1 (N8R8 on the bench; N4R2 targeted for production) | Controller — WiFi, dual I²S, deep sleep |
| TDK ICS-43434 (I²S MEMS mic) | Microphone — mounted as a separate satellite board, off the main PCB, for acoustic placement |
| MAX98357A (I²S class-D amp) | Speaker drive |
| 2× TI DRV8833 (dual H-bridge) | 3 motor channels — mouth, head, tail (all spring-return, unidirectional) |
| DIODES AP63203 buck (AP63201 populated as a fallback — see Known issues) | 6V → 3.3V logic/audio rail |
| 3× TI LM74700-Q1 + AOSS32334C FETs | Ideal-diode power-input protection — battery reverse-polarity, USB/motor-rail isolation |
| 2× 5A slow-blow fuses | Battery and DC-jack input protection |

`board.h` in the firmware is the single source of truth for the pin map.

**Workflow:**
```bash
open board/billy/billy.kicad_pro
/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli sch erc --format json --severity-all -o erc.json board/billy/billy.kicad_sch
```

---

## `src/firmware/` — ESP32-S3 firmware

Captures speech, calls the backend for STT → LLM → TTS, plays the reply, and drives the
mouth/head/tail motors in sync with it. Targets `esp32s3` (ESP-IDF v6.0.2); no persona or
conversation state lives on-device.

### Layering

| Layer | Files | Responsibility |
|---|---|---|
| app | `main.c` | `app_main()` wires everything together; `runloop_task` runs the `IDLE→ACTIVATE→LISTEN→THINK→SPEAK→IDLE` state machine. |
| transport | `net.c/.h` | The fish↔backend contract described above — STT direct, brain hop through the shim, TTS direct. The one layer that would change if the fish ever moved from direct HTTP to ESPHome/Home Assistant. |
| device | `audio.c/.h`, `motors.c/.h`, `activation.c/.h`, `sensors.c/.h`, `status_led.c/.h` | Fish-specific setup and algorithms built on the HAL: I²S mic/amp + mouth lip-sync envelope; 2× DRV8833 motor choreography; button/mode-switch/wake-word activation + deep sleep; photocell + Vmotor-sense reads; WS2812 status LED. |
| HAL | `hal.c/.h` | Generic, pin-parameterized primitives (GPIO, ADC1, LED strip, PWM, I²S, deep sleep) with no fish-specific naming and no `board.h` dependency of its own. |

`board.h` is the single source of truth for the pin map. `components/wakeword` is the on-device
wake-word detector (TensorFlow Lite Micro, ported from ESPHome's `micro_wake_word` component) —
see its own `models/ATTRIBUTION.md` for training provenance.

### Build

```bash
source ~/.espressif/tools/activate_idf_v6.0.2.sh
cd src/firmware
export WIFI_SSID="your-2.4GHz-ssid" WIFI_PASSWORD="your-password" BACKEND_HOST="your-host-or-ip"
idf.py set-target esp32s3      # once
idf.py build
idf.py -p <port> flash monitor
```
WiFi credentials and the backend host are build-environment variables injected as compile
definitions (`main/CMakeLists.txt`), never committed to source. Changing one needs
`idf.py reconfigure`. `./clean.sh` removes all generated build state.

### Debug

Step-debug with real breakpoints over the S3's native USB-Serial-JTAG, no extra hardware:
```bash
openocd -f board/esp32s3-builtin.cfg    # terminal 1
idf.py -p <port> gdb                    # terminal 2
```
The DevKitC-1 has two USB-C ports — flashing uses the UART/CP2102 port; OpenOCD needs the
separate native-USB port instead.

### Runtime behavior

`runloop_task` starts immediately on boot and idles waiting for activation; it does not block
on WiFi or the backend being reachable. Activation depends on the mode switch: **button mode**
(press-to-talk, deep-sleeps between turns) or **wake-word mode** (mic stays live, listens for
"hey billy"; a button press also works as a manual override). Flipping the switch mid-wait
preempts immediately. Runtime-tunable constants (VAD thresholds, photocell gate, motor timing,
HTTP timeouts) are fetched from the shim's `/v1/config` every loop cycle rather than compiled
in, with compiled-in fallbacks if the shim is unreachable.

---

## `src/shim/` — backend application-logic layer

The brain layer in front of `llama.cpp`. Holds the application logic that has to live off the
fish for it to stay a thin client:

- **persona** / system prompt — edit `text.py`, reload, no reflash;
- **conversation history / session state** — kept server-side, keyed by session id;
- **model quirks** — the `enable_thinking` request flag and `<think>`-block stripping;
- **markdown/emoji scrubbing** — the LLM still emits stray formatting that TTS would mangle;
- **sentence splitting + streaming** — emits clean sentences as they're ready so the caller can
  pipeline TTS for low first-audio latency;
- **runtime firmware config** (`fish_config.py`) — tunable firmware constants served over
  `GET /v1/config`, so they can be retuned without a reflash.

It is a text service — it never touches audio. STT and TTS are dumb audio↔text transforms the
caller calls directly.

### Protocol

| Call | Request | Response |
|---|---|---|
| `POST /v1/respond` | `{"session": "default", "text": "<user utterance>"}` | `text/event-stream`: one `data: {"sentence": ...}` per spoken sentence, then `data: [DONE]` |
| `POST /v1/reset` | `{"session": "default"}` | `{"reset": "<session>"}` |
| `GET /v1/config` | — | Runtime firmware-tunable constants |
| `GET /health` | — | `{status, llm_url, llm_model, llm_reachable, sessions}` |

### Run

```bash
cd src/shim
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/uvicorn billy_shim:app --host 0.0.0.0 --port 8000
```
Config via env: `BILLY_LLM_URL` (default `http://localhost:8080`), `BILLY_LLM_MODEL` (default
`Qwen3.5-9B`). Tests (no pytest): `.venv/bin/python test_text.py`.

### Deploy

Lives at `/opt/billy-shim` on the backend host, managed by the `billy-shim.service` systemd
unit (`deploy/systemd/billy-shim.service`).
```bash
rsync -a --exclude '.venv' --exclude '__pycache__' src/shim/ your-host-or-ip:/opt/billy-shim/
ssh your-host-or-ip systemctl restart billy-shim
```

---

## `src/client/` — CLI reference client (Python)

A command-line stand-in for the fish: drives the full backend pipeline (record → STT → shim →
TTS → play) from any host with a mic and speaker.

### Run

```bash
cd src/client
python3 -m venv .venv && . .venv/bin/activate && pip install -r requirements.txt
./billy_cli.py --host <backend-host>     # ports 8000/8081/8880 derived from host
```
Press **Enter** to start talking, speak, press **Enter** to stop. `Ctrl-C` quits.

`--voice <kokoro-voice>` overrides the voice for the whole session (browse the list at
`http://<backend-host>:8880/web`); otherwise each sentence uses the shim's own per-sentence
choice. `--session <id>` sets the shim conversation id; `--shim-url`/`--stt-url`/`--tts-url`
override individual endpoints. Uses the system default audio devices.

Each turn prints various latency measurements.

---

## `src/cli/` — CLI reference client (C)

A second, independent reference client for the same contract — same record → STT → shim → TTS →
play pipeline as the Python client, same flags, same latency breakdown, but built as a plain C
binary via CMake instead of a Python venv. It exists less because the Python client needed
replacing and more to size up what a from-scratch C port actually costs: which external
libraries a host build needs versus what's already solved for free on-device, and how much of
the firmware's own protocol code turns out to be portable as-is. That last part went further
than expected — `net.c`'s hand-rolled WAV header and JSON string extraction have zero ESP-IDF
dependency, so `src/cli` reuses the same techniques verbatim rather than pulling in a JSON
library.

External dependencies (none of this is vendored): `libcurl` (HTTP, mirrors `httpx`), `portaudio`
(mic/speaker I/O, mirrors `sounddevice`), `soxr` (output resampling, same library the Python
`soxr` package binds to). On macOS via MacPorts: `port install curl portaudio soxr`. `numpy` and
`soundfile` have no C-side equivalent needed — buffer handling is plain arrays, and the WAV
format in play (canonical PCM16) is simple enough to read/write by hand instead of linking
libsndfile.

### Build & run

```bash
cd src/cli
cmake -S . -B build && cmake --build build
./build/billy_cli --host <backend-host>     # ports 8000/8081/8880 derived from host
```
Press **Enter** to start talking, speak, press **Enter** to stop. `Ctrl-C` is a hard exit here —
unlike the Python client it doesn't catch the interrupt for a graceful shutdown message.

Same `--voice`/`--session`/`--shim-url`/`--stt-url`/`--tts-url` flags as the Python client, same
system-default audio devices. TTS audio is resampled to the output device's native rate in
software (soxr, `SOXR_HQ` quality) rather than by requesting a mismatched rate from PortAudio and
letting the OS mixer convert it — the latter produced audible static in testing.

---

## Backend engines

Three off-the-shelf inference engines run on the backend GPU host alongside the shim, all under
systemd (`Restart=always`, enabled at boot):

| Service | Port | Model | GPU |
|---|---|---|---|
| llama.cpp | 8080 | Qwen3.5-9B, Q8_0, non-thinking mode | yes |
| whisper.cpp | 8081 | `large-v3-turbo` | yes |
| Kokoro (Kokoro-FastAPI) | 8880 | voice `am_onyx` | yes |

All three plus the shim are expected to fit resident in ~14GB of VRAM.

---

## Known issues

- **Buck regulator EN is never gated.** Both AP63201 (currently populated) and AP63203
  (preferred once sourceable) have EN tied straight to VIN, so the buck runs continuously
  through deep sleep. AP63201's quiescent current (~291µA) alone exceeds the board's ≤100µA
  standby target by ~3x; AP63203 would bring it down to ~22µA. No fix designed yet.
- **Power-input ideal-diode legs can't be gated in deep sleep either.** Each LM74700-Q1 draws
  80µA typical and is part of the power path itself, so it can't be switched off by the very
  chip it feeds. That's roughly 160µA typical continuous across the two active legs (battery +
  USB), stacked on top of the buck issue above. No fix designed yet.