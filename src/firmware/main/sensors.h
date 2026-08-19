// Interpreted ADC readings: the photocell and Vmotor-sense divider -- oversampling and
// calibration/threshold math.
#pragma once
#include <stdbool.h>

// Brings up the shared ADC1 unit (hal_adc_init()), then a one-shot boot smoke-test read of each
// channel, logged for visibility. Call once, before any other sensors_* function.
void sensors_init(void);

// Averaged (8x) Vmotor (Vdrive) reading, converted to volts via a bench calibration. Current fit
// is only validated ~5.4-6.06V.
float sensors_read_vmotor_volts(void);

// True if the room is bright enough that a wake candidate should be treated as real rather than
// a false positive from a dark room. Logs the raw reading and the outcome each call. Threshold is
// a compile-time constant. Bench-calibrated raw readings ran ~400-1200 across dim-to-bright room
// light, dropped to 120-150 with only monitor glow (room lights off), and 40-85 with only ambient
// window light; the threshold (20) sits below that window-only band with margin.
bool sensors_photocell_bright_enough(void);

// Rev.0 debug only: reclaims BOARD_MISC_GPIO from ADC mode and drives it high or low, for the
// bare scope-probe header on that board. Do not call on Rev.1 hardware. The same pin there is
// BOARD_VMOTOR_ADC, wired to the Vdrive divider, and this will fight it.
void sensors_set_misc_gpio_debug(bool level);
