# Billy fish firmware (ESP32-S3)

The on-fish firmware — the custom-firmware / direct-HTTP path. Builds and runs for `esp32s3` on a
bare ESP32-S3-WROOM-1 N8R8, wired on a breadboard (not installed in the fish toy). WiFi, backend
transport, mic/amp audio I/O, activation (button + mode switch + wake-word detection), motor
drive (mouth/head/tail lip-sync and choreography), button-mode deep sleep, and the full
conversational turn loop are all real and bench-verified — see Status below. The two hardware
findings previously open (head deflection, buck power under combined motor+logic load) are
resolved (spent-battery sag, not a firmware/board issue).

## Layering

Kept as three layers so the deferred orchestrator decision (custom HTTP vs. ESPHome/HA) only
ever touches the transport seam:

| Layer | Files | Responsibility |
|---|---|---|
| **app** | `main.c` | `app_main()` wires the layers together; `runloop_task` runs the `IDLE→ACTIVATE→LISTEN→THINK→SPEAK→IDLE` state machine. |
| **transport** | `net.c/.h` | The fish↔backend contract — STT (whisper), brain (shim), TTS (Kokoro). The orchestrator seam. WiFi join/reconnect is async and unbounded (a dedicated task supervises it for the app's whole runtime). |
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

The turn loop starts immediately on boot — it doesn't wait for WiFi to join or the backend to
become reachable (look for `got IP …` in the log once it does). It idles waiting for activation
(button press or wake word, depending on the mode switch) and runs a full conversational turn on
each one; a turn that needs the network before WiFi/the backend are up just fails that one turn
(error tone, back to idle) rather than blocking startup. A dropped connection at any point during
runtime — not just at boot — is retried with backoff, indefinitely, by a dedicated task.

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

## Status

**Real and bench-verified:**
- WiFi join, async and non-blocking (the turn loop starts immediately rather than waiting for an
  IP); a dedicated task retries a dropped connection with backoff, indefinitely, for the app's
  whole runtime rather than giving up after a fixed number of attempts.
- Mic/amp I²S audio: energy-VAD utterance capture, amp playback.
- Full transport: STT direct to whisper.cpp, the brain hop to the shim (SSE-streamed sentences),
  TTS direct to Kokoro — the same three-call contract `client/billy_cli.py` demonstrates.
- Activation: `BOARD_BUTTON` and `BOARD_MODE_SW` are bare jumpers on the breadboard (not yet a
  real button/switch); debounced press-to-wake, and a live mode-switch flip preempts an in-progress
  wait instantly in either direction. A button press also works as a manual override while in
  wake-word mode, not just in button mode.
- Wake-word detection (`components/wakeword`): a trained "hey billy" microWakeWord model on
  TensorFlow Lite Micro, always-listening in wake-word mode. See
  `components/wakeword/models/ATTRIBUTION.md` for training details and the exact manifest values.
- Motors: all three (mouth/head/tail) wired to 2× DRV8833 breakouts and bench-verified. `hal.c`
  drives all three `IN1` pins with LEDC PWM off one shared timer, `IN2` held low as plain GPIO
  (each motor is unidirectional/spring-return), and `nSLEEP` gates both DRV8833s together (parked
  between turns, enabled on first drive). A bench duty sweep found the mechanism can't usefully
  track duty proportionally (barely moves below ~65% — see `WIRING.md` §6.1), so head and tail
  are pure on/off gates (no plausible use case for a partial head/tail gesture) driven at full
  duty. Mouth gets a third level: CLOSED / MID (~80% duty) / OPEN (100%), off two envelope
  thresholds on the RMS of the audio actually being played — the bench sweep found ~80% is a
  real, visually distinct partial deflection on this mechanism (not just a weaker copy of 100%),
  so quieter/plain speech reads as a smaller mouth movement than a loud/emphasized one, instead of
  everything snapping the same wide-open amount. The tail flaps once on any
  activation; the head raises on the first sentence that actually has audio (not earlier, so it
  lines up with when sound actually starts rather than LLM/TTS latency) and holds until the reply
  is fully done, then relaxes. `nFAULT` is read and logged (not acted on) after tail flaps and
  head relax; a deliberate forced-stall test was considered and rejected — the motor shafts are
  friction-fit to their gears, so a forced stall risks the gears more than it proves the driver's
  OCP works (`WIRING.md` §6.2). Trusting the DRV8833 datasheet's OCP/thermal/UVLO protection
  instead of empirically triggering it.
- Photocell: read once at boot (`fish_hal_init()`), and the ADC unit is kept alive (not torn
  down) so `fish_hal_read_photocell()` is available any time — no consumer wired up yet by
  design, kept warm for a future hook (novelty wake / presence / ambient light).
- Deep sleep (button mode only): `fish_hal_prepare_sleep()` mutes the amp, clears the status LED,
  and holds both through the sleep (`gpio_hold_en()` + the ESP32-S3-required global
  `gpio_deep_sleep_hold_en()`, or they'd float back up once the digital domain powers down —
  undoing the DRV8833s' `nSLEEP`-parked µA state) before arming `ext1` wake on `BOARD_BUTTON` and
  calling `esp_deep_sleep_start()`. Since deep sleep is a full chip reset, `runloop_task` checks
  `fish_hal_woke_from_wake_event()` on boot and starts at `FISH_ACTIVATE` instead of `FISH_IDLE`
  when the reboot was caused by that button press — otherwise it would immediately call
  `fish_hal_prepare_sleep()` again and re-sleep without ever using the press that woke it.
  Bench-verified: button press → reboot (`rst:0x5 (DSLEEP)`) → prompt tone → LISTEN, repeatedly.
  WAKEWORD mode is unchanged (mic has to stay live, so it never sleeps). The actual standby
  *current* measurement is waived for now — it needs the bare module (§3.1), not the DevKitC,
  which doesn't exist yet; deep sleep itself is verified functionally.

Both hardware findings previously tracked here (head deflection, buck brownout under motor load)
are **resolved** — both root-caused to spent-battery sag, not a firmware or board issue; see
`../BUGS.md` and `WIRING.md` §6.1/§9.6.
