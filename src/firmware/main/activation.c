#include "activation.h"
#include "hal.h"
#include "board.h"
#include "wakeword.h"
#include "sensors.h"
#include "status_led.h"
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
    // Both active-low with internal pull-ups — a floating jumper reads high, grounding it reads
    // low. The button also has an EXTERNAL pull-up (board.h) for the deep-sleep wake path; the
    // internal pull here is redundant with it but harmless, and is what the mode switch relies on
    // alone.
    hal_gpio_init_input(BOARD_BUTTON, true);
    hal_gpio_init_input(BOARD_MODE_SW, true);
    ESP_LOGI(TAG, "init: activation inputs (button, mode switch) configured");
}

// Active-low: the jumper grounded to GND reads 0 = pressed.
bool activation_button_pressed(void)
{
    return !hal_gpio_get(BOARD_BUTTON);
}

bool activation_button_press_debounced(void)
{
    if (!activation_button_pressed()) return false;
    vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
    return activation_button_pressed();
}

typedef enum
{
    WAKE_MODE_BUTTON,
    WAKE_MODE_WAKEWORD,
} wake_mode_t;

static const char *wake_mode_name(wake_mode_t m)
{
    return m == WAKE_MODE_BUTTON ? "BUTTON" : "WAKEWORD";
}

static wake_mode_t current_wake_mode(void)
{
    // BOARD_MODE_SW has an internal pull-up: floating (high) selects the low-power default,
    // BUTTON; grounding it selects hands-free WAKEWORD. Read fresh each turn so moving the jumper
    // takes effect immediately.
    return hal_gpio_get(BOARD_MODE_SW) ? WAKE_MODE_BUTTON : WAKE_MODE_WAKEWORD;
}

static bool button_mode_active(void)   { return current_wake_mode() == WAKE_MODE_BUTTON; }
static bool wakeword_mode_active(void) { return current_wake_mode() == WAKE_MODE_WAKEWORD; }

// Block until the button reads released, polling `keep_waiting` so a caller can bail out (e.g. on
// a mode-switch flip) instead of blocking indefinitely. This is what stops a held/stuck jumper
// from immediately counting as a fresh press when a wait function starts. Returns false if
// keep_waiting() ever returns false; true once released (or if it was never pressed).
static bool wait_for_button_release(bool (*keep_waiting)(void))
{
    while (activation_button_pressed())
    {
        if (!keep_waiting()) return false;
        vTaskDelay(pdMS_TO_TICKS(WAKE_POLL_MS));
    }
    return true;
}

// BUTTON mode: block until a fresh, debounced press. If the jumper is already grounded from the
// previous turn, wait for release first so a held wire can't auto-advance every turn — each turn
// then needs a deliberate press edge. Returns true on a press; false if the mode switch moved off
// BUTTON, so the runloop can re-dispatch to the other wake source without waiting a whole turn.
static bool wait_for_button(void)
{
    ESP_LOGI(TAG, "wake(button): waiting for a press on GPIO %d (ground the jumper to press)",
             BOARD_BUTTON);
    if (!wait_for_button_release(button_mode_active)) return false;
    for (;;)
    {
        if (!button_mode_active()) return false;
        if (activation_button_press_debounced())
        {
            ESP_LOGI(TAG, "wake(button): press detected");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(WAKE_POLL_MS));
    }
}

// WAKEWORD mode: block until "hey billy" (see components/wakeword/models/ATTRIBUTION.md) is heard
// on the continuously-running mic, OR the button is pressed as a manual override. Waits for the
// button to be released first, same as wait_for_button, so a jumper already held when this mode
// is entered doesn't immediately fire an override. Reads the mic WAKEWORD_STEP_SAMPLES (10 ms) at
// a time via audio.c and feeds it to the detector; each step also checks the mode switch and
// button, so a flip or press preempts within one step. Returns true on detection/press; false if
// the mode switch moved off WAKEWORD.
static bool wait_for_wakeword(void)
{
    ESP_LOGI(TAG, "wake(wakeword): listening for the wake word (or a button press)");
    if (!wait_for_button_release(wakeword_mode_active)) return false;
    wakeword_reset();

    int32_t raw[WAKEWORD_STEP_SAMPLES];
    int16_t pcm[WAKEWORD_STEP_SAMPLES];

    for (;;)
    {
        if (!wakeword_mode_active()) return false;

        if (activation_button_press_debounced())
        {
            ESP_LOGI(TAG, "wake(wakeword): button press detected (manual override)");
            return true;
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
            return true;
        }
    }
}

// BUTTON mode: real deep sleep, not a polling loop -- this is the whole point of the mode (months
// of standby on 4xC NiMH). Deep sleep is a full chip reset: nothing after hal_deep_sleep_enter()
// runs, and the next code to execute is app_main() from scratch on wake. The digital domain (and
// its GPIO config/levels) is lost across that reset except for pins explicitly held -- SD_MODE,
// nSLEEP, and the status LED's VDD gate all need to stay exactly where they are (muted / parked /
// unpowered) for the whole sleep, or the amp, DRV8833s, and LED would come back up floating
// instead (nSLEEP floating high would undo the DRV8833s' uA-standby state, the actual point of
// this milestone). ESP32-S3 needs the *global* hal_deep_sleep_hold_enable() for a per-pin
// hal_gpio_hold_enable() to actually survive deep sleep (not just light-sleep/reset) -- see
// motors_init()'s hal_gpio_hold_disable() call, which releases these same holds on wake.
static void enter_deep_sleep_for_button_wake(void)
{
    ESP_LOGI(TAG, "prepare sleep (button mode): entering deep sleep, wake on GPIO %d press or "
                  "GPIO %d mode-switch flip", BOARD_BUTTON, BOARD_MODE_SW);

    led_prepare_for_sleep();
    audio_mute_for_sleep();
    motors_hold_for_sleep();
    hal_deep_sleep_hold_enable();

    // Both BOARD_BUTTON and BOARD_MODE_SW have an external pull-up (board.h).
    hal_deep_sleep_enable_ext1_wakeup((1ULL << BOARD_BUTTON) | (1ULL << BOARD_MODE_SW));
    hal_deep_sleep_enter();   // does not return
}

wake_pin_t activation_deep_sleep_wake_pin(void)
{
    // The only deep-sleep wakeup source we ever arm is ext1 on BOARD_BUTTON and BOARD_MODE_SW
    // (see enter_deep_sleep_for_button_wake above), so no ext1 cause means a cold boot/flash/reset.
    if (!hal_deep_sleep_woke_on_ext1()) return WAKE_PIN_NONE;

    // hal_deep_sleep_ext1_wake_pins() reports which of the armed pins were actually low,
    // distinguishing which one caused this particular reboot. Both reads are of latched
    // wake-status bits, not consumed on read, so calling this more than once (e.g. once here,
    // again inside activation_boot_cause()) is safe and cheap.
    uint64_t low_pins = hal_deep_sleep_ext1_wake_pins();
    if (low_pins & (1ULL << BOARD_BUTTON))  return WAKE_PIN_BUTTON;
    if (low_pins & (1ULL << BOARD_MODE_SW)) return WAKE_PIN_MODE_SW;
    return WAKE_PIN_NONE;
}

boot_cause_t activation_boot_cause(void)
{
    switch (activation_deep_sleep_wake_pin())
    {
        case WAKE_PIN_BUTTON:
            // A dark room rules the reboot out as a likely false positive from the flaky button
            // contact rather than a deliberate press.
            return sensors_photocell_bright_enough("deep-sleep boot: button") ? BOOT_BUTTON : BOOT_NONE;
        case WAKE_PIN_MODE_SW:
            ESP_LOGI(TAG, "deep-sleep boot: mode switch flipped");
            return BOOT_MODE_CHANGE;
        case WAKE_PIN_NONE:
        default:
            return BOOT_NONE;
    }
}

void activation_prepare_sleep(void)
{
    motors_park();   // regardless of wake mode

    if (current_wake_mode() == WAKE_MODE_BUTTON)
    {
        enter_deep_sleep_for_button_wake();   // does not return
    }

    // Hands-free: the mic must stay live for the wake-word detector, so we don't sleep.
    ESP_LOGI(TAG, "prepare sleep (wakeword mode): motors parked; mic stays live for detection — no sleep");
}

bool activation_wait_for_wake(void)
{
    // Read the current wake mode and run the correct wake detection.
    wake_mode_t mode = current_wake_mode();
    ESP_LOGI(TAG, "wait for wake — mode=%s", wake_mode_name(mode));
    bool woke = (mode == WAKE_MODE_BUTTON) ? wait_for_button() : wait_for_wakeword();
    if (!woke)
    {
        // The wait functions return false as soon as they see the mode switch flip (checked
        // every poll tick / audio step).
        ESP_LOGI(TAG, "wake: mode switch flipped — returning to idle");
        return false;
    }
    return sensors_photocell_bright_enough(wake_mode_name(mode));
}
