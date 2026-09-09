// Peripherals gated by BOARD_PERIPHERALS_EN (board.h): the status LED and the photocell +
// Vmotor-sense dividers all share one low-side FET gate for their power/GND return, so they share
// one init/sleep lifecycle here.
#pragma once
#include <stdbool.h>

typedef enum
{
    LED_STATUS_BOOT,       // white  — power-on / booting, before the turn loop starts
    LED_STATUS_IDLE,       // green  — waiting for a wake event (button or wake word)
    LED_STATUS_LISTEN,     // blue   — activated, capturing the utterance
    LED_STATUS_THINK,      // cyan   — transcribing / waiting on the backend's reply
    LED_STATUS_SPEAK,      // violet — voicing the reply
    LED_STATUS_ERROR,      // red    — fatal init failure
} led_status_t;

// Drives BOARD_PERIPHERALS_EN high, brings up ADC1 and the RMT/WS2812 driver, shows
// LED_STATUS_BOOT, and takes one smoke-test reading of each sensor channel (logged only). Call
// once, before any other peripherals_* function.
void peripherals_init(void);

// Sets the LED status (color)
void peripherals_set_led_status(led_status_t status);

// Averaged (8x) Vmotor (Vdrive) reading, converted to volts via a bench calibration. Current fit
// is only validated ~5.4-6.06V.
float peripherals_read_vmotor_volts(void);

// True if the room is bright enough that a wake candidate should be treated as real rather than
// a false positive from a dark room. Logs the raw reading and the outcome each call. Threshold is
// a compile-time constant. Bench-calibrated raw readings ran ~400-1200 across dim-to-bright room
// light, dropped to 120-150 with only monitor glow (room lights off), and 40-85 with only ambient
// window light; the threshold (20) sits below that window-only band with margin.
bool peripherals_photocell_bright_enough(void);

// BUTTON-mode deep sleep only: clears the LED (a WS2812 otherwise latches its last color and
// keeps displaying it with no power drawn), then cuts + holds BOARD_PERIPHERALS_EN low -- zero
// current for the LED and both sense dividers for the whole sleep.
void peripherals_prepare_for_sleep(void);
