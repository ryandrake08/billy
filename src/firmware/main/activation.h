// Activation: mode switch + wake sources (button / wake word), and BUTTON-mode deep sleep.
// Device-level module built on hal.h's generic hal_gpio_*/hal_deep_sleep_* primitives, composed
// with motors.c/audio.c/status_led.c (each owns its own "prepare for sleep" step) and sensors.c
// (photocell brightness gate).
//
// Two activation modes, selected by the physical mode switch (BOARD_MODE_SW):
//   BUTTON   — press-to-talk. The fish can deep-sleep and wake on the button GPIO for months of
//              standby. Lowest power; needs a deliberate press.
//   WAKEWORD — hands-free "hey billy". The CPU + mic stay powered to listen continuously, so this
//              mode cannot deep-sleep — that is the standby-power tradeoff.
//
// The mode switch and button are bench-wired as bare jumpers on their GPIOs (grounding = "switch
// selected" / "button pressed"). WAKEWORD mode runs a real detector (components/wakeword) on a
// trained "Hey Billy" model (see components/wakeword/models/ATTRIBUTION.md).
#pragma once
#include <stdbool.h>

// Configures the button and mode-switch GPIO inputs. Call once, before any other activation_*
// function.
void activation_init(void);

// Raw instantaneous button level and a single-shot debounced press check (must still read pressed
// after a short settle delay). Exposed for audio.c, which polls these to let a button press
// interrupt an in-progress mic capture -- see audio_capture_utterance().
bool activation_button_pressed(void);
bool activation_button_press_debounced(void);

// Which physical pin caused an EXT1 deep-sleep wake, if any -- a raw hardware fact, unlike
// activation_boot_cause() below, which additionally validates a button pin against the photocell
// (and therefore against fish_config_get(), which may not have its freshest value yet -- see
// activation_boot_cause()'s doc comment). Exists so a caller can act on "was this a button press"
// before that validation runs, without creating a circular dependency on boot_cause's own,
// photocell-dependent result.
typedef enum
{
    WAKE_PIN_NONE,     // not an EXT1 wake (cold boot/flash/reset)
    WAKE_PIN_BUTTON,   // BOARD_BUTTON was low at wake
    WAKE_PIN_MODE_SW,  // BOARD_MODE_SW was low at wake
} wake_pin_t;

wake_pin_t activation_deep_sleep_wake_pin(void);

// Why this boot exists: a deep-sleep reboot caused by the button or the mode switch, or no such
// cause at all (a cold boot/flash/reset).
typedef enum
{
    BOOT_NONE,        // cold boot/flash/reset, or a button reboot rejected by the photocell
    BOOT_BUTTON,       // rebooted by a (photocell-confirmed) button press
    BOOT_MODE_CHANGE, // rebooted because the mode switch flipped while asleep
} boot_cause_t;

// The reason this boot exists -- see boot_cause_t. The runloop uses BOOT_BUTTON to skip straight
// to ACTIVATE instead of re-entering IDLE (which would otherwise immediately call
// activation_prepare_sleep() again and go right back to sleep without ever using the press that
// caused it). BOOT_MODE_CHANGE and BOOT_NONE both re-enter IDLE, which puts the chip right back
// to sleep unless the switch is now in WAKEWORD mode.
//
// For a button pin, this validates against the photocell (fish_config_get()->
// photocell_wake_threshold) *before* the runloop has had any chance to fetch fresh config -- so
// that check always sees the compiled-in default unless a caller forces a fetch first, which
// means checking activation_deep_sleep_wake_pin() (above) rather than this function, since this
// function's own result depends on the very config a forced fetch would change.
boot_cause_t activation_boot_cause(void);

// IDLE: park the motors. In BUTTON mode this mutes the amp, cuts the LED, and enters real deep
// sleep on BOARD_BUTTON -- it does not return; the chip fully resets and re-runs app_main() on
// wake. In WAKEWORD mode it returns normally (the mic must stay live for detection, so there's no
// sleep).
void activation_prepare_sleep(void);

// Block until an activation event (button press, or wake word in always-on mode) passes the
// photocell brightness gate. A single attempt, not a retry loop -- returns false both when the
// mode switch flips while waiting and when a real wake candidate is rejected as too dark, so the
// caller always re-enters IDLE (re-running activation_prepare_sleep() and net_fetch_config())
// between attempts rather than retrying silently inside this call. Returns true only for an
// activation that's both real and photocell-approved.
bool activation_wait_for_wake(void);
