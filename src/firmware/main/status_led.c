#include "status_led.h"
#include "hal.h"
#include "board.h"
#include "esp_log.h"

static const char *TAG = "led";

// Dim RGB triples, not 0-255 — a WS2812 at full brightness is eye-searing a few inches away on a
// bench. Indexed directly by led_status_t.
static const uint8_t STATUS_COLORS[][3] = {
    [LED_STATUS_BOOT]   = { 15, 15,  15 },  // white
    [LED_STATUS_IDLE]   = {  0, 20,   0 },  // green
    [LED_STATUS_LISTEN] = {  0,  0,  20 },  // blue
    [LED_STATUS_THINK]  = {  0, 20,  20 },  // cyan
    [LED_STATUS_SPEAK]  = { 15,  0,  20 },  // violet
    [LED_STATUS_ERROR]  = { 20,  0,   0 },  // red
};

void led_init(void)
{
    // The LED's supply is switched (low-side FET, BOARD_AWAKE_EN) rather than tied straight to
    // 3V3, so power has to actually be up before the first RMT frame goes out, or the boot-status
    // color would be clocked into a dark LED and never seen. This pin also gates the photocell
    // divider's return (board.h) -- driving it here covers both
    hal_gpio_init_output(BOARD_AWAKE_EN, true);

    // Release any hold left over from a deep sleep the chip just woke from (led_prepare_for_sleep
    // holds this pin low through sleep) -- level is already the desired 1 (awake), so this
    // can't glitch it.
    hal_gpio_hold_disable(BOARD_AWAKE_EN);

    hal_led_init(BOARD_STATUS_LED);
    hal_led_clear();
    led_set_status(LED_STATUS_BOOT);
    ESP_LOGI(TAG, "init: status LED ready");
}

void led_set_status(led_status_t status)
{
    const uint8_t *c = STATUS_COLORS[status];
    hal_led_show_rgb(c[0], c[1], c[2]);
}

void led_prepare_for_sleep(void)
{
    // Clear to black *before* cutting BOARD_AWAKE_EN below, so DIN is already idling low (not
    // mid-toggle) at the moment VDD goes away -- a WS2812 otherwise latches whatever color it
    // last received and keeps displaying it with no further refresh needed.
    hal_led_clear();
    hal_gpio_set(BOARD_AWAKE_EN, false);   // cuts the LED's VDD *and* the photocell divider
    hal_gpio_hold_enable(BOARD_AWAKE_EN);
}
