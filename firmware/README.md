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
| **transport** | `net.c/.h` | The fish↔homelab contract — STT (whisper), brain (shim), TTS (Kokoro). The orchestrator seam. |
| **hardware** | `hal.c/.h`, `board.h` | Mic/amp I²S, 2× DRV8833 motors, button/switch/photocell. `board.h` is the pin map (§4.1). |

The transport layer mirrors the three calls the CLI reference client makes
(`client/billy_cli.py`), so the fish reimplements the same proven contract.

## Build

```bash
source /Users/ryan/.espressif/tools/activate_idf_v6.0.2.sh   # activate ESP-IDF v6.0.2
cd firmware
idf.py set-target esp32s3      # once (writes sdkconfig from sdkconfig.defaults)
idf.py build                   # no board required
```

Flashing + serial monitor (`idf.py -p <port> flash monitor`) needs the ESP32-S3 board over
USB — added in Stage 2 once the hardware arrives.

## Status

Scaffold only. Next: real WiFi + `esp_http_client` in `net.*`, then I²S/motor drivers in
`hal.*` on the bench (Stage 3). Add the corresponding `REQUIRES` to `main/CMakeLists.txt` as
each driver lands.
