// Interpreted ADC readings: the photocell and Vmotor-sense divider. Device-level module built on
// hal.h's generic hal_adc_read() -- oversampling and calibration/threshold math live here, not in
// the HAL (see .devdocs/WIRING.md §3h-2 for the Vmotor calibration data this is built from).
#pragma once
#include <stdbool.h>

// Brings up the shared ADC1 unit (hal_adc_init()), then a one-shot boot smoke-test read of each
// channel, logged for visibility. Call once, before any other sensors_* function.
void sensors_init(void);

// Averaged (8x) Vmotor (Vdrive) reading, converted to volts via a bench calibration fit against
// the board.h divider (R13/R15). That fit is only validated ~5.4-6.06V -- see
// .devdocs/WIRING.md §3h-2 before trusting a reading outside that band (e.g. for a low-battery
// cutoff).
float sensors_read_vmotor_volts(void);

// True if the room is bright enough that a wake candidate should be treated as real rather than
// a false positive from a dark room. Logs the raw reading and the outcome each call -- `context`
// names the caller for that log line. Threshold is runtime-tunable (fish_config.h) --
// bench-calibrated raw readings ran ~400-1200 across dim-to-bright room light, dropped to 120-150
// with only monitor glow (room lights off), and 40-85 with only ambient window light; the default
// of 50 sits within the window-only band.
bool sensors_photocell_bright_enough(const char *context);

// Rev.0 debug only: reclaims BOARD_MISC_GPIO from ADC mode and drives it high or low, for the
// bare scope-probe header on that board. Do not call on Rev.1 hardware -- the same pin there is
// BOARD_VMOTOR_ADC, wired to the Vdrive divider, and this will fight it. Safe to call repeatedly;
// each call reconfigures the pad fresh. After calling this, sensors_read_vmotor_volts() will
// return stale/invalid readings until hal_adc_init() re-runs (not automatic) -- fine on Rev.0,
// since there's no real divider there for it to read anyway.
void sensors_set_misc_gpio_debug(bool level);
