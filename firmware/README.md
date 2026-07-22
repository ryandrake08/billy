# Billy fish firmware (ESP32-S3)

The on-fish firmware — the custom-firmware / direct-HTTP path (`SCOPING.md` §8.1). Right now
it's a **scaffold with stubbed I/O**: the full runloop and the layer seams exist and build for
`esp32s3`, but the hardware drivers and networking are stubs (no board yet). It runs the whole
turn shape on-target, logging each step, so control flow is verified before Stage 3 wiring.

## Layering (`SCOPING.md` §8.1)

Kept as three layers so the deferred orchestrator decision (custom HTTP vs. ESPHome/HA) only
ever touches the transport seam:

| Layer | Files | Responsibility |
|---|---|---|
| **app** | `main.c`, `runloop.c/.h` | The `IDLE→ACTIVATE→LISTEN→THINK→SPEAK→IDLE` state machine (§6). |
| **transport** | `net.c/.h` | The fish↔backend contract — STT (whisper), brain (shim), TTS (Kokoro). The orchestrator seam. |
| **hardware** | `hal.c/.h`, `board.h` | Mic/amp I²S, 2× DRV8833 motors, button/switch/photocell. `board.h` is the pin map (§4.1). |

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
`hello backend — … HTTP 200`). Then it runs the stubbed turn loop.

## Status

WiFi + backend reachability are real (Stage 2.2); the runloop I/O is still stubbed. Next:
I²S/motor drivers in `hal.*` and the real STT/brain/TTS HTTP calls in `net.*` on the bench
(Stage 3; breadboard wiring in the repo-root). Add the corresponding `REQUIRES` to
`main/CMakeLists.txt` as each driver lands.
