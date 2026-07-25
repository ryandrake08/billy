# Billy fish firmware (ESP32-S3)

The on-fish firmware — the custom-firmware / direct-HTTP path. Builds and runs for `esp32s3` on a
bare ESP32-S3-WROOM-1 N8R8, wired on a breadboard (not installed in the fish toy). WiFi, backend
transport, mic/amp audio I/O, activation (button + mode switch + wake-word detection), and the
full conversational turn loop are all real and bench-verified. Motors (mouth/head/tail lip-sync
and choreography) and deep-sleep low-power mode are still stubbed — see Status below.

## Layering

Kept as three layers so the deferred orchestrator decision (custom HTTP vs. ESPHome/HA) only
ever touches the transport seam:

| Layer | Files | Responsibility |
|---|---|---|
| **app** | `main.c`, `runloop.c/.h` | The `IDLE→ACTIVATE→LISTEN→THINK→SPEAK→IDLE` state machine. |
| **transport** | `net.c/.h` | The fish↔backend contract — STT (whisper), brain (shim), TTS (Kokoro). The orchestrator seam. |
| **hardware** | `hal.c/.h`, `board.h`, `components/wakeword` | Mic/amp I²S, 2× DRV8833 motors, button/switch/photocell, wake-word detection. `board.h` is the pin map. |

The transport layer mirrors the three calls the CLI reference client makes
(`client/billy_cli.py`), so the fish reimplements the same proven contract.

## Build

```bash
source /Users/ryan/.espressif/tools/activate_idf_v6.0.2.sh   # activate ESP-IDF v6.0.2
cd firmware

# WiFi credentials and the backend host come from the build environment, never source control
# (main/CMakeLists.txt injects them as compile definitions). BACKEND_HOST is required; export
# these BEFORE set-target — it runs a CMake configure that fails without BACKEND_HOST.
export WIFI_SSID="your-2.4GHz-ssid"
export WIFI_PASSWORD="your-password"
export BACKEND_HOST="your-host-or-ip"

idf.py set-target esp32s3      # once (configures + writes sdkconfig from sdkconfig.defaults)
idf.py build                   # without WiFi creds it warns and can't join WiFi; BACKEND_HOST is required
```

Changing an exported value needs `idf.py reconfigure` (CMake reads the environment at configure
time). Flash + watch the boot log:

```bash
idf.py -p <port> flash monitor
```

On boot the fish joins WiFi and GETs the shim's `/health` on `your-host-or-ip:8000` — the "hello
backend" proof that toolchain and network path both work (look for `got IP …` and
`hello backend — … HTTP 200`). It then idles waiting for activation (button press or wake word,
depending on the mode switch) and runs a full conversational turn on each one.

## Status

**Real and bench-verified:**
- WiFi join + backend health check.
- Mic/amp I²S audio: energy-VAD utterance capture, amp playback with mouth-motor sync deferred
  (playback itself works; motor sync doesn't exist yet since there are no motors wired).
- Full transport: STT direct to whisper.cpp, the brain hop to the shim (SSE-streamed sentences),
  TTS direct to Kokoro — the same three-call contract `client/billy_cli.py` demonstrates.
- Activation: `BOARD_BUTTON` and `BOARD_MODE_SW` are bare jumpers on the breadboard (not yet a
  real button/switch); debounced press-to-wake, and a live mode-switch flip preempts an in-progress
  wait instantly in either direction.
- Wake-word detection (`components/wakeword`): a trained "hey billy" microWakeWord model on
  TensorFlow Lite Micro, always-listening in wake-word mode. See
  `components/wakeword/models/ATTRIBUTION.md` for training details and the exact manifest values.

**Still stubbed:** the three motors (mouth lip-sync, head raise, tail flap — logged as intent, no
GPIO/PWM driver yet) and deep-sleep low-power mode for button-only standby. Add the corresponding
`REQUIRES` (e.g. `esp_driver_ledc` for motor PWM) to `main/CMakeLists.txt` as each driver lands.
