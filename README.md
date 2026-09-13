# Smart Mouth Billy Bass LLM voice assistant

Billy is a Big Mouth Billy Bass novelty toy converted into an **offline, LAN-only** voice
assistant. An ESP32-S3 inside the fish captures speech, drives the motors, and plays back
audio; a separate GPU box does speech-to-text, LLM inference, and text-to-speech. There is no
internet dependency anywhere in the pipeline.

The fish's' persona, conversation history, text cleanup, and configuraion all live on the
backend, in an business-logic "shim" application in front of the LLM. The fish just captures
audio, makes three HTTP calls per turn, plays the reply, and animates the mouth/head/tail in
sync with it.

## Architecture

```
        FISH (ESP32-S3)                                BACKEND (LAN)
  ┌──────────────────────────┐                  ┌───────────────────────────┐
  │ mic → capture utterance  │─── utterance ───▶│ whisper.cpp        :8081  │
  │                          │◀───── text ──────│                           │
  |                          |                  |                           |    ┌─────────────────┐
  │ text → shim              │────── text ─────▶│ shim (persona,     :8000  │──▶ | llama.cpp :8080 |
  │                          │◀─── response ────│  history, text cleaning)  │    └─────────────────┘
  │                          │                  │                           │
  │ sentence → TTS           │──── sentence ───▶│ Kokoro             :8880  │
  │                          │◀──── audio ──────│                           │
  │ amp + motors             │                  └───────────────────────────┘
  └──────────────────────────┘
```

Three calls per conversational turn:

| Call | Target | Request → Response |
|---|---|---|
| STT | whisper.cpp `:8081`, direct | `POST /inference` (multipart wav, `response_format=verbose_json`, `language=auto`) → `{"text", "detected_language", ...}` |
| brain | shim `:8000` | `POST /v1/respond` `{session, text, language}` → SSE stream of `{"sentence", "voice"}` |
| TTS | Kokoro `:8880`, direct | `POST /v1/audio/speech` `{model, input, voice}` → wav bytes |

STT and TTS are dumb audio↔text transforms called **directly**; only the brain hop goes through
the shim, which is a text-only service that never touches audio. The shim streams sentences as
the LLM generates them, so the caller can synthesize and play sentence 1 while sentence 2 is
still being generated — this pipelining is what keeps time-to-first-audio low.

**`language=auto` on the STT call is required.** whisper.cpp's server defaults each request to its launch-time `-l` flag (`en` on this deployment); decoding non-English audio as English silently produces an English *translation* instead of a transcript.

**`language` on `/v1/respond`** is whisper's `detected_language` (a lowercase English name, e.g.
`"spanish"`), forwarded as-is by the client.

There are three implementations of this protocol, and they should be indentical: the reference clients
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
| `src/cli/` | CLI reference client (C) — built against libcurl/PortAudio/soxr. |
| `src/tests/` | End-to-end integration test for the multi-language STT→shim→LLM pipeline. |

---

## `board/` — Custom PCB

KiCad project at `board/billy/`. Replaces the toy's stock control board with a single board
carrying the ESP32-S3, motor drivers, amp, buck regulator, and power-input protection —
designed to fit inside the original toy's chassis (~70×50mm).

| Part | Role |
|---|---|
| ESP32-S3-WROOM-1-N4R2 | Controller — WiFi, dual I²S, deep sleep |
| TDK ICS-43434 (I²S MEMS mic) | Microphone — mounted as a separate satellite board, off the main PCB, for acoustic placement |
| MAX98357A (I²S class-D amp) | Speaker drive |
| 2× TI DRV8833 (dual H-bridge) | 3 motor channels — mouth, head, tail (all spring-return, unidirectional) |
| DIODES AP63203 buck | 6V → 3.3V logic/audio rail |
| AOS AO3401A / AO3400A FETs (battery reverse-polarity protection, USB/motor-rail isolation pass FET, status LED + photocell/Vmotor-sense divider gating) | Power-input protection and deep-sleep peripheral gating |
| Analog Devices LTC4412 | Ideal-diode controller — motor rail vs. USB arbitration into the buck |
| MCC B5817W Schottky | Passive ideal diode — USB VBUS into the buck |
| 2× 5A slow-blow fuses | Battery and DC-jack input protection |

The KiCad schematic and PCB are authoritative for hardware connectivity and layout. Firmware
`board.h` mirrors the schematic's GPIO assignments and must stay synchronized with it.

**Workflow:**
```bash
open board/billy/billy.kicad_pro
kicad-cli sch erc --format json --severity-all -o erc.json board/billy/billy.kicad_sch
kicad-cli pcb drc --format json --severity-all --schematic-parity -o drc.json board/billy/billy.kicad_pcb
```

---

## `src/firmware/` — ESP32-S3 firmware

Captures speech, calls the backend for STT → LLM → TTS, plays the reply, and drives the
mouth/head/tail motors in sync with it. Targets `esp32s3` (tested with ESP-IDF v6.0.2).

### Layering

| Layer | Files | Responsibility |
|---|---|---|
| app | `main.c` | `app_main()` wires everything together; `runloop_task` runs the `IDLE→ACTIVATE→LISTEN→THINK→SPEAK→IDLE` state machine. |
| transport | `net.c/.h` | The fish↔backend contract described above — STT direct, brain hop through the shim, TTS direct. |
| device | `audio.c/.h`, `motors.c/.h`, `activation.c/.h`, `peripherals.c/.h` | Fish-specific setup and algorithms built on the HAL: I²S mic/amp + mouth lip-sync envelope; 2× DRV8833 motor choreography with `nFAULT` telemetry and deferred Vmotor capture; button/mode-switch/wake-word activation + deep sleep; WS2812 status LED + photocell/Vmotor-sense reads, all gated by one shared enable line. |
| HAL | `hal.c/.h` | Generic, pin-parameterized primitives (GPIO, ADC1, LED strip, PWM, I²S, deep sleep) with no fish-specific naming and no `board.h` dependency of its own. |

`board.h` is the firmware representation of the schematic pin map. `components/wakeword` is the on-device
wake-word detector (TensorFlow Lite Micro, ported from ESPHome's `micro_wake_word` component). See its own
`models/ATTRIBUTION.md` for training provenance.

### Build

```bash
source ~/.espressif/tools/activate_idf_v6.0.2.sh
cd src/firmware
export WIFI_SSID="your-2.4GHz-ssid" WIFI_PASSWORD="your-password" BACKEND_HOST="your-backend-hostname-or-ip"
idf.py set-target esp32s3      # once
idf.py build
idf.py -p <port> flash monitor
```
WiFi credentials and the backend host are build-environment variables injected as compile
definitions (`main/CMakeLists.txt`). Changing one needs `idf.py reconfigure`. `./clean.sh`
removes all generated build state.

### Debug

Step-debug with breakpoints over the S3's native USB-Serial-JTAG:
```bash
openocd -f board/esp32s3-builtin.cfg    # terminal 1
idf.py -p <port> gdb                    # terminal 2
```
The DevKitC-1 has two USB ports — flashing uses the UART/CP2102 port; OpenOCD needs the
separate native-USB port instead. The production board has a USB port and a three pin
header (TTL level) to one of the ESP32's built-in UARTs. Only use a USB-to-UART cable that
uses 3.3V with this header! In addition to the direct-to-UART header, the production board
has a two pin header for firmware flashing. Jumping the header during a reset causes the chip
to enter UART download (flashing) mode instead of booting normally from flash memory.
The UART/FLASH headers are intended to be a backup in case flashing over USB fails, and the
header can be left unpopulated if unused.

### Runtime behavior

`runloop_task` starts immediately on boot and idles waiting for activation; it does not block
on WiFi or the backend being reachable. Activation depends on the mode switch: **button mode**
(press-to-talk, deep-sleeps between turns) or **wake-word mode** (mic stays live, listens for
"hey billy"; a button press also works as a manual override). Flipping the switch mid-wait
preempts immediately. Runtime-tunable constants (VAD thresholds, mouth/tail timing, and HTTP
timeouts) are fetched from the shim's `/v1/config` every loop cycle rather than compiled
in, with compiled-in fallbacks if the shim is unreachable. Each capture's VAD threshold adapts
to the room's ambient noise level, measured during the pre-capture drain window, so a noisy
room still reads as silence once the user stops talking. The DRV8833 `nFAULT` line is logged but
does not gate motor commands because it asserts unreliably on the board; the drivers' independent
silicon overcurrent, thermal, and undervoltage protection remains active.

---

## `src/shim/` — backend application-logic layer

The brain layer in front of `llama.cpp`. Holds the application logic that has to live off the
fish for it to stay a thin client:

- **persona** / system prompt — edit `text.py`, reload;
- **conversation history / session state** — kept server-side, keyed by session id;
- **model quirks** — the `enable_thinking` request flag and `<think>`-block stripping;
- **markdown/emoji scrubbing** — the LLM still emits stray formatting that TTS would mangle;
- **sentence splitting + streaming** — emits clean sentences as they're ready so the caller can
  pipeline TTS for low first-audio latency;
- **per-language voice routing** (`text.py`'s `LANGUAGE_VOICES`) — English (`am_onyx`) plus
  Spanish, Brazilian Portuguese, Mandarin, Japanese, Hindi, and Italian, picked from the caller's
  detected language and overridable per sentence for an explicit switch mid-reply;
- **runtime firmware config** (`fish_config.py`) — tunable firmware constants served over
  `GET /v1/config`, so they can be retuned without a reflash.

### Protocol

| Call | Request | Response |
|---|---|---|
| `POST /v1/respond` | `{"session": "default", "text": "<user utterance>", "language": "english"}` | `text/event-stream`: one `data: {"sentence": ..., "voice": ...}` per spoken sentence, then `data: [DONE]` |
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
`Qwen3.5-9B`). Tests use plain asserts rather than pytest:

```bash
.venv/bin/python test_text.py
.venv/bin/python test_config.py
.venv/bin/python test_session.py
```

### Deploy

Shim program can live anywhere. Recommend using your OS's init system to start and manage it.

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

`--input-wav <path>` skips the mic entirely: runs one turn on that WAV file's bytes (16 kHz mono,
same as the mic path itself records) and exits — for testing the same utterance repeatedly
without re-recording it by hand each time.

Each turn prints various latency measurements.

---

## `src/cli/` — CLI reference client (C)

A second, independent reference client for the same contract — same record → STT → shim → TTS →
play pipeline as the Python client, same flags, same latency breakdown, but built as a plain C
binary via CMake instead of a Python venv.

External dependencies (none of this is vendored): `libcurl` (HTTP, mirrors `httpx`), `portaudio`
(mic/speaker I/O, mirrors `sounddevice`), `soxr` (output resampling, same library the Python
`soxr` package binds to). On macOS via MacPorts: `port install curl portaudio soxr`.

### Build & run

```bash
cd src/cli
cmake -S . -B build && cmake --build build
./build/billy_cli --host <backend-host>     # ports 8000/8081/8880 derived from host
```
Press **Enter** to start talking, speak, press **Enter** to stop. `Ctrl-C` quits, same as the
Python client.

Same `--voice`/`--session`/`--shim-url`/`--stt-url`/`--tts-url` flags as the Python client, same
system-default audio devices. TTS audio is resampled to the output device's native rate in
software (soxr, `SOXR_HQ` quality).

---

## `src/tests/` — end-to-end integration test

Exercises the  STT→shim→LLM chain for every supported language. For each language, a
"golden" WAV asking Billy to repeat a fixed phrase is transcribed by real whisper.cpp, then sent
to a shim, checking the reply comes back in the right voice and language.

```bash
cd src/tests
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
./.venv/bin/python test_language_integration.py     # needs src/shim/.venv already set up
```

The golden WAVs in `golden/` are gitignored: `generate_golden.py` makes them with Microsoft's
free `edge-tts`. The test does this itself the first time golden/ is missing a file.

---

## Backend engines

Three off-the-shelf inference engines run on the backend GPU host alongside the shim, all under
systemd (`Restart=always`, enabled at boot):

| Service | Port | Model | GPU |
|---|---|---|---|
| llama.cpp | 8080 | Qwen3.5-9B, Q8_0, non-thinking mode | yes |
| whisper.cpp | 8081 | `large-v3` | yes |
| Kokoro (Kokoro-FastAPI) | 8880 | `am_onyx` + 6 other language voices (see `src/shim/`) | yes |

All three plus the shim are expected to fit resident in ~14GB of VRAM.

---

## License

Source code and documentation are licensed under [Apache License 2.0](LICENSE). The
trained wake-word model `src/firmware/components/wakeword/models/hey_billy.tflite` is
explicitly excluded; see [NOTICE](NOTICE) and its
[`ATTRIBUTION.md`](src/firmware/components/wakeword/models/ATTRIBUTION.md).
