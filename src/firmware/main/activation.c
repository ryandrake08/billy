#include "activation.h"
#include "hal.h"
#include "board.h"
#include "wakeword.h"
#include "peripherals.h"
#include "motors.h"
#include "audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "activation";

#define BUTTON_DEBOUNCE_MS   40    // press must persist this long to count (contact bounce)
#define WAKE_POLL_MS         20    // poll interval for the mode switch (button mode) / button debounce

void activation_init(void)
{
    // Both active-low with internal pull-ups — a floating GPIO reads high, grounding it reads
    // low. The button also has an EXTERNAL pull-up (board.h) for the deep-sleep wake path; the
    // internal pull here is redundant with it but harmless, and is what the mode switch relies on
    // alone.
    hal_gpio_init_input(BOARD_BUTTON, true);
    hal_gpio_init_input(BOARD_MODE_SW, true);
    ESP_LOGI(TAG, "init: activation inputs (button, mode switch) configured");
}

// Active-low: the GPIO grounded to GND reads 0 = pressed.
static bool button_is_pressed(void)
{
    return !hal_gpio_get(BOARD_BUTTON);
}

bool activation_button_press_debounced(void)
{
    if (!button_is_pressed()) return false;
    vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
    return button_is_pressed();
}

typedef enum
{
    WAKE_MODE_BUTTON,
    WAKE_MODE_WAKEWORD,
} wake_mode_t;

// BOARD_MODE_SW has an internal pull-up: floating (high) selects the low-power default,
// BUTTON; grounding it selects hands-free WAKEWORD.
static wake_mode_t current_wake_mode(void)
{
#if BOARD_UNWIRED == 1
    // Force wakeword mode when unwired, to keep device awake when bench testing
    return WAKE_MODE_WAKEWORD;
#else
    return hal_gpio_get(BOARD_MODE_SW) ? WAKE_MODE_BUTTON : WAKE_MODE_WAKEWORD;
#endif
}

// Function callbacks available to be used by wait_for_button_release()
static bool button_mode_active(void)   { return current_wake_mode() == WAKE_MODE_BUTTON; }
static bool wakeword_mode_active(void) { return current_wake_mode() == WAKE_MODE_WAKEWORD; }
static bool always_keep_waiting(void)  { return true; }

// Block until the button reads released, polling `keep_waiting` so a caller can bail out (e.g. on
// a mode-switch flip) instead of blocking indefinitely. This is what stops a held/stuck button
// from immediately counting as a fresh press when a wait function starts. Returns false if
// keep_waiting() ever returns false; true once released (or if it was never pressed).
static bool wait_for_button_release(bool (*keep_waiting)(void))
{
    while (button_is_pressed())
    {
        if (!keep_waiting()) return false;
        vTaskDelay(pdMS_TO_TICKS(WAKE_POLL_MS));
    }
    return true;
}

void activation_wait_for_button_release(void)
{
    wait_for_button_release(always_keep_waiting);
}

// BUTTON mode: block until a fresh, debounced press. Waits for the button to be released first.
// Returns ACTIVATION_BUTTON on a press; ACTIVATION_MODE_SW if the mode switch moved off BUTTON.
static activation_event_t wait_for_button(void)
{
    ESP_LOGI(TAG, "wake(button): waiting for a press on GPIO %d", BOARD_BUTTON);

    // First, wait until the button is released (so we return ACTIVATION_BUTTON on a real button press)
    if (!wait_for_button_release(button_mode_active)) return ACTIVATION_MODE_SW;
    for (;;)
    {
        // If we ever switch out of button mode, tell caller that we saw a mode switch
        if (!button_mode_active()) return ACTIVATION_MODE_SW;

        if (activation_button_press_debounced())
        {
            ESP_LOGI(TAG, "wake(button): press detected");
            return ACTIVATION_BUTTON;
        }
        vTaskDelay(pdMS_TO_TICKS(WAKE_POLL_MS));
    }
}

// WAKEWORD mode: block until "hey billy" (see components/wakeword/models/ATTRIBUTION.md) is heard
// on the continuously-running mic, OR the button is pressed as a manual override. Waits for the
// button to be released first. Reads the mic WAKEWORD_STEP_SAMPLES (10 ms) at a time via audio.c
// and feeds it to the detector. Returns ACTIVATION_WAKEWORD on detection, ACTIVATION_BUTTON on
// the manual override, or ACTIVATION_MODE_SW if the mode switch moved off WAKEWORD.
static activation_event_t wait_for_wakeword(void)
{
    ESP_LOGI(TAG, "wake(wakeword): listening for the wake word (or a button press)");

    // First, wait until the button is released (so we return ACTIVATION_BUTTON on a real button press)
    if (!wait_for_button_release(wakeword_mode_active)) return ACTIVATION_MODE_SW;

    wakeword_reset();   // fresh listening session -- clear any stale window from a prior one

    int32_t raw[WAKEWORD_STEP_SAMPLES];
    int16_t pcm[WAKEWORD_STEP_SAMPLES];

    for (;;)
    {
        // If we ever switch out of wakeword mode, tell caller that we saw a mode switch
        if (!wakeword_mode_active()) return ACTIVATION_MODE_SW;

        if (activation_button_press_debounced())
        {
            ESP_LOGI(TAG, "wake(wakeword): button press detected (manual override)");
            return ACTIVATION_BUTTON;
        }

        size_t bytes_read = 0;
        if (audio_mic_read_raw(raw, sizeof raw, &bytes_read) != ESP_OK)
            continue;
        size_t n = bytes_read / sizeof(int32_t);
        if (n < WAKEWORD_STEP_SAMPLES) continue;

        // 24-bit sample MSB-first in the 32-bit slot — same conversion as VAD capture (top 16 bits).
        for (size_t i = 0; i < WAKEWORD_STEP_SAMPLES; i++)
            pcm[i] = (int16_t) (raw[i] >> 16);

        if (wakeword_feed(pcm, WAKEWORD_STEP_SAMPLES))
        {
            ESP_LOGI(TAG, "wake(wakeword): detected");
            return ACTIVATION_WAKEWORD;
        }
    }
}

wake_pin_t activation_deep_sleep_wake_pin(void)
{
    // The only deep-sleep wakeup source we ever arm is ext1 on BOARD_BUTTON and BOARD_MODE_SW
    if (!hal_deep_sleep_woke_on_ext1()) return WAKE_PIN_NONE;

    // hal_deep_sleep_ext1_wake_pins() reports which of the armed pins were actually low,
    // distinguishing which one caused this particular reboot. Both reads are of latched
    // wake-status bits, not consumed on read, so calling this more than once is safe and cheap.
    uint64_t low_pins = hal_deep_sleep_ext1_wake_pins();
    if (low_pins & (1ULL << BOARD_BUTTON))  return WAKE_PIN_BUTTON;
    if (low_pins & (1ULL << BOARD_MODE_SW)) return WAKE_PIN_MODE_SW;
    return WAKE_PIN_NONE;
}

void activation_prepare_sleep(void)
{
    motors_park();   // regardless of wake mode

    if (current_wake_mode() == WAKE_MODE_BUTTON)
    {
        // BUTTON mode: enter deep sleep. Deep sleep is a full chip reset: nothing after hal_deep_sleep_enter()
        // runs, and the next code to execute is app_main() on wake.
        ESP_LOGI(TAG, "prepare sleep (button mode): entering deep sleep, wake on GPIO %d press or "
                    "GPIO %d mode-switch flip", BOARD_BUTTON, BOARD_MODE_SW);

        peripherals_prepare_for_sleep();
        audio_mute_for_sleep();
        motors_hold_for_sleep();
        hal_deep_sleep_hold_enable();

        // Both BOARD_BUTTON and BOARD_MODE_SW have an external pull-up (board.h).
        hal_deep_sleep_enable_ext1_wakeup((1ULL << BOARD_BUTTON) | (1ULL << BOARD_MODE_SW));
        hal_deep_sleep_enter();   // does not return
    }

    // Hands-free: the mic must stay live for the wake-word detector, so we don't sleep.
    ESP_LOGI(TAG, "prepare sleep (wakeword mode): motors parked; mic stays live for detection — no sleep");
}

activation_event_t activation_wait(void)
{
    // Read the current wake mode and run the correct wake detection.
    wake_mode_t mode = current_wake_mode();
    ESP_LOGI(TAG, "wait for wake — mode=%s", mode == WAKE_MODE_BUTTON ? "BUTTON" : "WAKEWORD");

    // Call the correct wait function
    activation_event_t event = (mode == WAKE_MODE_BUTTON) ? wait_for_button() : wait_for_wakeword();

    switch (event)
    {
        case ACTIVATION_BUTTON:
            ESP_LOGI(TAG, "activation_wait: button press");
            break;
        case ACTIVATION_WAKEWORD:
            ESP_LOGI(TAG, "activation_wait: wake word detected");
            break;
        case ACTIVATION_MODE_SW:
            // wait_for_button()/wait_for_wakeword() return this as soon as they see the mode
            // switch flip (checked every poll tick / audio step).
            ESP_LOGI(TAG, "activation_wait: mode switch flipped — returning to idle");
            break;
        case ACTIVATION_NONE:
            ESP_LOGI(TAG, "activation_wait: no event");
            break;
    }
    return event;
}
