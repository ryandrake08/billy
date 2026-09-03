#include "peripherals.h"
#include "hal.h"
#include "board.h"
#include "esp_log.h"

static const char *TAG = "peripherals";

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

void peripherals_init(void)
{
    // BOARD_PERIPHERALS_EN gates the LED's VDD *and* the photocell/Vmotor-sense dividers' GND
    // returns (board.h) -- drive it high first so every peripheral below is actually powered
    // before anything reads or drives one.
    hal_gpio_init_output(BOARD_PERIPHERALS_EN, true);

    // Release any hold left over from a deep sleep the chip just woke from
    // (peripherals_prepare_for_sleep holds this pin low through sleep) -- level is already the
    // desired 1 (awake), so this can't glitch it.
    hal_gpio_hold_disable(BOARD_PERIPHERALS_EN);

    hal_adc_init();
    int photocell_raw = hal_adc_read(BOARD_PHOTOCELL_ADC);
    ESP_LOGI(TAG, "init: photocell ready (raw=%d)", photocell_raw);

    // No consumer yet on either revision of this pad -- Rev.0 boards wire it as a bare
    // scope-probe header instead of the Vdrive divider, so this reads near 0 there (see board.h).
    float vmotor_v = peripherals_read_vmotor_volts();
    ESP_LOGI(TAG, "init: Vmotor sense ready (%.2fV)", vmotor_v);

    hal_led_init(BOARD_STATUS_LED);
    hal_led_clear();
    peripherals_set_led_status(LED_STATUS_BOOT);
    ESP_LOGI(TAG, "init: status LED ready");
}

void peripherals_set_led_status(led_status_t status)
{
    const uint8_t *c = STATUS_COLORS[status];
    hal_led_show_rgb(c[0], c[1], c[2]);
}

// Averaged over a handful of reads -- bench data showed ~1-1.6% scatter (rail ripple under motor
// load and/or ADC sample jitter) on a single raw read.
#define VMOTOR_SAMPLE_COUNT 8

// Empirically calibrated (bench PSU + multimeter). raw = SLOPE * volts + OFFSET.
#define VMOTOR_CAL_SLOPE_COUNTS_PER_VOLT  567.6f
#define VMOTOR_CAL_OFFSET_COUNTS        (-354.3f)

float peripherals_read_vmotor_volts(void)
{
    long sum = 0;
    for (int i = 0; i < VMOTOR_SAMPLE_COUNT; i++)
    {
        sum += hal_adc_read(BOARD_VMOTOR_ADC);
    }
    float raw_avg = (float) sum / VMOTOR_SAMPLE_COUNT;
    return (raw_avg - VMOTOR_CAL_OFFSET_COUNTS) / VMOTOR_CAL_SLOPE_COUNTS_PER_VOLT;
}

#if BOARD_UNWIRED == 1
#define PHOTOCELL_WAKE_THRESHOLD -1
#else
#define PHOTOCELL_WAKE_THRESHOLD 20
#endif

bool peripherals_photocell_bright_enough(void)
{
    int raw = hal_adc_read(BOARD_PHOTOCELL_ADC);
    bool ok = raw >= PHOTOCELL_WAKE_THRESHOLD;
    ESP_LOGI(TAG, "photocell: raw=%d threshold=%d -> %s", raw, PHOTOCELL_WAKE_THRESHOLD, ok ? "ok" : "too dark, ignoring");
    return ok;
}

void peripherals_prepare_for_sleep(void)
{
    // Clear to black *before* cutting BOARD_PERIPHERALS_EN below, so DIN is already idling low
    // (not mid-toggle) at the moment VDD goes away -- a WS2812 otherwise latches whatever color
    // it last received and keeps displaying it with no further refresh needed.
    hal_led_clear();
    hal_gpio_set(BOARD_PERIPHERALS_EN, false);   // cuts LED VDD *and* both sense dividers' GND
    hal_gpio_hold_enable(BOARD_PERIPHERALS_EN);
}

void peripherals_set_misc_gpio_debug(bool level)
{
    hal_gpio_reset(BOARD_MISC_GPIO);
    hal_gpio_init_output(BOARD_MISC_GPIO, level);
}
