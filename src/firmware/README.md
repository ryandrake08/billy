# Billy fish firmware (ESP32-S3)

The on-fish firmware. Captures speech, calls the backend for STT → LLM → TTS, plays the reply,
and drives the mouth/head/tail motors in sync with it. Runs on an ESP32-S3-WROOM-1 N8R8, target
`esp32s3`, no persona or conversation state on-device — see the top-level `CLAUDE.md` for how this
fits into the rest of the project.

## Layering

Three layers, each with a single responsibility:

| Layer | Files | Responsibility |
|---|---|---|
| **app** | `main.c` | `app_main()` wires the layers together; `runloop_task` runs the `IDLE→ACTIVATE→LISTEN→THINK→SPEAK→IDLE` state machine. |
| **transport** | `net.c/.h` | The fish↔backend contract — STT (whisper), brain (shim), TTS (Kokoro). WiFi join/reconnect is async and unbounded, supervised by a dedicated task for the app's whole runtime. |
| **hardware** | `hal.c/.h`, `board.h`, `components/wakeword` | Mic/amp I²S, 2× DRV8833 motors, button/switch/photocell, wake-word detection. `board.h` is the pin map. |

The transport layer mirrors the three calls the CLI reference client makes
(`src/client/billy_cli.py`), so the fish reimplements the same contract.

## Build

```bash
source ~/.espressif/tools/activate_idf_v6.0.2.sh   # activate ESP-IDF v6.0.2
cd src/firmware

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

Use `./clean.sh` to remove all generated/fetched build state (`build/`, `.cache/`,
`managed_components/`, `dependencies.lock`, `sdkconfig`, `sdkconfig.old`) and start fresh.

## Debug

Step-debug with real breakpoints over the S3's native USB-Serial-JTAG, no extra hardware:

```bash
openocd -f board/esp32s3-builtin.cfg    # terminal 1 — leave running
idf.py -p <port> gdb                    # terminal 2
```

The DevKitC-1 has **two** USB-C ports — flashing via `idf.py flash` (above) uses the **UART**
port (the CP2102 bridge), but OpenOCD needs the separate **USB** port instead, wired straight to
the S3's native USB-Serial-JTAG peripheral. Plugging into the wrong port shows up as OpenOCD
failing with `could not find or open device!` — check `system_profiler SPUSBDataType` (macOS) for
a `303a:1001` device to confirm you're on the right one. That port also carries USB power, so
debug sessions have the same constraint as USB flashing: keep the battery/buck disconnected while
it's plugged in.

## Runtime behavior

`runloop_task` starts immediately on boot and idles in `FISH_IDLE` waiting for activation — it
does not wait for WiFi to join or the backend to become reachable first (look for `got IP …` in
the log once WiFi is up). A turn that needs the network before it's ready just fails that one turn
(error tone, back to idle) rather than blocking startup; a dropped connection at any point during
runtime is retried indefinitely with backoff by a dedicated task.

Activation depends on `BOARD_MODE_SW`:
- **Button mode** — a `BOARD_BUTTON` press starts a turn. Between turns the fish deep-sleeps (mic
  and status LED off, motors parked) and wakes on the next button press.
- **Wake-word mode** — the mic stays live and a turn starts on the "hey billy" phrase
  (`components/wakeword`, on-device TensorFlow Lite Micro). A button press also works here as a
  manual override.

Flipping the mode switch mid-wait preempts an in-progress wait immediately in either direction.

Each turn drives all three motors (mouth/head/tail) in sync with the reply audio and plays it
through the amp; the photocell is read at boot and available via `fish_hal_read_photocell()`, with
no consumer wired up yet. See `board.h` for the pin map and `WIRING.md` for driver and mechanism
details.
