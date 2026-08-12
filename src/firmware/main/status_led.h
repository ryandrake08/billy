// Status LED: single WS2812, one color per app state.
#pragma once

typedef enum
{
    LED_STATUS_BOOT,       // white  — power-on / booting, before the turn loop starts
    LED_STATUS_IDLE,       // green  — waiting for a wake event (button or wake word)
    LED_STATUS_LISTEN,     // blue   — activated, capturing the utterance
    LED_STATUS_THINK,      // cyan   — transcribing / waiting on the backend's reply
    LED_STATUS_SPEAK,      // violet — voicing the reply
    LED_STATUS_ERROR,      // red    — fatal init failure
} led_status_t;

// Powers the LED's VDD gate, brings up the RMT/WS2812 driver, and shows LED_STATUS_BOOT.
void led_init(void);

void led_set_status(led_status_t status);

// BUTTON-mode deep sleep only: clears the LED (a WS2812 otherwise latches its last color and
// keeps displaying it with no power drawn) and cuts + holds its VDD gate low, so it draws zero
// current for the whole sleep.
void led_prepare_for_sleep(void);
