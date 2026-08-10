#include "sensors.h"
#include "hal.h"
#include "board.h"
#include "fish_config.h"
#include "esp_log.h"

static const char *TAG = "sensors";

void sensors_init(void)
{
    hal_adc_init();

    int photocell_raw = hal_adc_read(BOARD_PHOTOCELL_ADC);
    ESP_LOGI(TAG, "init: photocell ready (raw=%d)", photocell_raw);

    // No consumer yet on either revision of this pad -- Rev.0 boards wire it as a bare
    // scope-probe header instead of the Vdrive divider, so this reads near 0 there (see board.h).
    float vmotor_v = sensors_read_vmotor_volts();
    ESP_LOGI(TAG, "init: Vmotor sense ready (%.2fV)", vmotor_v);
}

// Averaged over a handful of reads -- bench data showed ~1-1.6% scatter (rail ripple under motor
// load and/or ADC sample jitter) on a single raw read; see .devdocs/WIRING.md §3h-2.
#define VMOTOR_SAMPLE_COUNT 8

// Empirically calibrated (bench PSU + multimeter, .devdocs/WIRING.md §3h-2) against the board.h
// R13/R15 (100k/75k) divider: raw = SLOPE * volts + OFFSET. Runs ~6.7% steeper than the
// nominal-3.3V-ADC theoretical slope -- most likely ESP32 ADC1's real characteristic curve at
// 12 dB attenuation, not resistor tolerance. Only validated 5.40-6.06V.
#define VMOTOR_CAL_SLOPE_COUNTS_PER_VOLT  567.6f
#define VMOTOR_CAL_OFFSET_COUNTS        (-354.3f)

float sensors_read_vmotor_volts(void)
{
    long sum = 0;
    for (int i = 0; i < VMOTOR_SAMPLE_COUNT; i++)
    {
        sum += hal_adc_read(BOARD_VMOTOR_ADC);
    }
    float raw_avg = (float) sum / VMOTOR_SAMPLE_COUNT;
    return (raw_avg - VMOTOR_CAL_OFFSET_COUNTS) / VMOTOR_CAL_SLOPE_COUNTS_PER_VOLT;
}

bool sensors_photocell_bright_enough(const char *context)
{
    int threshold = fish_config_get()->photocell_wake_threshold;
    int raw = hal_adc_read(BOARD_PHOTOCELL_ADC);
    bool ok = raw >= threshold;
    ESP_LOGI(TAG, "photocell (%s): raw=%d threshold=%d -> %s",
             context, raw, threshold, ok ? "ok" : "too dark, ignoring");
    return ok;
}

void sensors_set_misc_gpio_debug(bool level)
{
    hal_gpio_reset(BOARD_MISC_GPIO);
    hal_gpio_init_output(BOARD_MISC_GPIO, level);
}
