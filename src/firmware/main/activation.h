// Activation: mode switch + wake sources (button / wake word), and BUTTON-mode deep sleep.
//
// Two activation modes, selected by the physical mode switch (BOARD_MODE_SW):
//   BUTTON   — press-to-talk. The fish can deep-sleep and wake on the button GPIO for months of
//              standby. Lowest power; needs a deliberate press.
//   WAKEWORD — hands-free "hey billy". The CPU + mic stay powered to listen continuously, so this
//              mode cannot deep-sleep — that is the standby-power tradeoff.
#pragma once
#include <stdbool.h>

// Configures the button and mode-switch GPIO inputs. Call once, before any other activation_*
// function.
void activation_init(void);

// Single-shot debounced press check (must still read pressed after a short settle delay).
bool activation_button_press_debounced(void);

// Block until the button reads released.
void activation_wait_for_button_release(void);

// Which physical pin caused an EXT1 deep-sleep wake, or NONE if it was not a wake from deep-sleep
typedef enum
{
    WAKE_PIN_NONE,     // not an EXT1 wake (cold boot/flash/reset)
    WAKE_PIN_BUTTON,   // BOARD_BUTTON was low at wake
    WAKE_PIN_MODE_SW,  // BOARD_MODE_SW was low at wake
} wake_pin_t;

wake_pin_t activation_deep_sleep_wake_pin(void);

// IDLE: park the motors. In BUTTON mode this mutes the amp, cuts the LED, and enters real deep
// sleep on BOARD_BUTTON. It does not return; the chip fully resets and re-runs app_main() on
// wake. In WAKEWORD mode it returns normally (the mic must stay live for detection, so there's no
// sleep).
void activation_prepare_sleep(void);

// A wake candidate reported by activation_wait() below. ACTIVATION_NONE is currently unused --
// reserved for a caller that needs to distinguish "nothing happened" from an explicit event.
typedef enum
{
    ACTIVATION_NONE,
    ACTIVATION_BUTTON,     // a button press, in either mode (BUTTON, or a WAKEWORD manual override)
    ACTIVATION_WAKEWORD,   // the wake word was detected (WAKEWORD mode only)
    ACTIVATION_MODE_SW,    // the mode switch flipped while waiting
} activation_event_t;

// Block until an activation candidate: a button press, a detected wake word (WAKEWORD mode), or
// the mode switch flipping while waiting.
activation_event_t activation_wait(void);
