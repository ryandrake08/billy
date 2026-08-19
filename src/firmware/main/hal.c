// True low-level HAL -- see hal.h. Every function here is a thin wrapper around one ESP-IDF
// driver concept, parameterized by pin/channel; nothing here knows what a "motor" or "mic" is.
#include "hal.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "led_strip.h"

// --- GPIO ----------------------------------------------------------------------------------------

void hal_gpio_reset(int pin)
{
    gpio_reset_pin(pin);
}

void hal_gpio_init_output(int pin, bool initial_level)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(pin, initial_level);
}

void hal_gpio_init_input(int pin, bool pull_up)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

bool hal_gpio_get(int pin)
{
    return gpio_get_level(pin) != 0;
}

void hal_gpio_set(int pin, bool level)
{
    gpio_set_level(pin, level);
}

void hal_gpio_hold_enable(int pin)
{
    gpio_hold_en(pin);
}

void hal_gpio_hold_disable(int pin)
{
    gpio_hold_dis(pin);
}

void hal_gpio_add_falling_interrupt(int pin, hal_gpio_interrupt_handler_t handler, void *arg)
{
    static bool isr_service_installed;
    if (!isr_service_installed)
    {
        ESP_ERROR_CHECK(gpio_install_isr_service(0));
        isr_service_installed = true;
    }
    ESP_ERROR_CHECK(gpio_set_intr_type(pin, GPIO_INTR_NEGEDGE));
    ESP_ERROR_CHECK(gpio_isr_handler_add(pin, handler, arg));
}

// --- ADC1 ------------------------------------------------------------------------------------------

static adc_oneshot_unit_handle_t s_adc1;

void hal_adc_init(void)
{
    adc_oneshot_unit_init_cfg_t cfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&cfg, &s_adc1));
}

int hal_adc_read(int gpio_pin)
{
    adc_unit_t unit;
    adc_channel_t channel;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(gpio_pin, &unit, &channel));
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc1, channel, &chan_cfg));
    int raw = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(s_adc1, channel, &raw));
    return raw;
}

// --- LED strip -------------------------------------------------------------------------------------

static led_strip_handle_t s_led_strip;

void hal_led_init(int gpio_pin)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = gpio_pin,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,   // 10 MHz — standard RMT tick rate for WS2812 timing
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_led_strip));
}

void hal_led_show_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(s_led_strip, 0, r, g, b);
    led_strip_refresh(s_led_strip);
}

void hal_led_clear(void)
{
    led_strip_clear(s_led_strip);
}

// --- PWM (LEDC) ------------------------------------------------------------------------------------
// One timer shared by every channel -- this board never needs two different PWM frequencies at
// once, so a second hal_pwm_init_timer() call (a second timer_num) isn't exposed until something
// actually needs it.

#define PWM_TIMER LEDC_TIMER_0
#define PWM_MODE  LEDC_LOW_SPEED_MODE   // esp32s3 has no high-speed LEDC mode

void hal_pwm_init_timer(int freq_hz, int resolution_bits)
{
    ledc_timer_config_t cfg = {
        .speed_mode      = PWM_MODE,
        .duty_resolution = (ledc_timer_bit_t) resolution_bits,
        .timer_num       = PWM_TIMER,
        .freq_hz         = freq_hz,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&cfg));
}

void hal_pwm_init_channel(int channel, int gpio_pin)
{
    ledc_channel_config_t cfg = {
        .gpio_num   = gpio_pin,
        .speed_mode = PWM_MODE,
        .channel    = (ledc_channel_t) channel,
        .timer_sel  = PWM_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&cfg));
}

void hal_pwm_set_duty(int channel, uint32_t duty)
{
    ledc_set_duty(PWM_MODE, (ledc_channel_t) channel, duty);
    ledc_update_duty(PWM_MODE, (ledc_channel_t) channel);
}

// --- I2S -------------------------------------------------------------------------------------------

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;

static i2s_data_bit_width_t bit_width_of(int bits)
{
    return bits == 16 ? I2S_DATA_BIT_WIDTH_16BIT : I2S_DATA_BIT_WIDTH_32BIT;
}

void hal_i2s_tx_init(const hal_i2s_config_t *cfg)
{
    // auto_clear_after_cb: the hardware zeroes each DMA buffer once it's sent and nothing new has
    // replaced it, so the channel plays silence whenever hal_i2s_tx_write() isn't actively feeding
    // it -- without ever needing to stop the clock. The only TX mode this board uses.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear_after_cb = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(cfg->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bit_width_of(cfg->bit_width),
                                                         cfg->stereo ? I2S_SLOT_MODE_STEREO
                                                                     : I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = cfg->bclk_pin,
            .ws   = cfg->ws_pin,
            .dout = cfg->dout_pin,
            .din  = cfg->din_pin,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
}

esp_err_t hal_i2s_tx_write(const void *data, size_t len, size_t *out_written)
{
    return i2s_channel_write(s_tx, data, len, out_written, portMAX_DELAY);
}

void hal_i2s_rx_init(const hal_i2s_config_t *cfg)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(cfg->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bit_width_of(cfg->bit_width),
                                                         cfg->stereo ? I2S_SLOT_MODE_STEREO
                                                                     : I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = cfg->bclk_pin,
            .ws   = cfg->ws_pin,
            .dout = cfg->dout_pin,
            .din  = cfg->din_pin,
        },
    };
    if (cfg->left_slot_only)
    {
        std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    }
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
}

esp_err_t hal_i2s_rx_read(void *buf, size_t len, size_t *out_read)
{
    return i2s_channel_read(s_rx, buf, len, out_read, portMAX_DELAY);
}

// --- Deep sleep --------------------------------------------------------------------------------

void hal_deep_sleep_hold_enable(void)
{
    gpio_deep_sleep_hold_en();
}

void hal_deep_sleep_enable_ext1_wakeup(uint64_t pin_mask)
{
    esp_sleep_enable_ext1_wakeup_io(pin_mask, ESP_EXT1_WAKEUP_ANY_LOW);
}

void hal_deep_sleep_enter(void)
{
    esp_deep_sleep_start();
}

bool hal_deep_sleep_woke_on_ext1(void)
{
    return (esp_sleep_get_wakeup_causes() & (1 << ESP_SLEEP_WAKEUP_EXT1)) != 0;
}

uint64_t hal_deep_sleep_ext1_wake_pins(void)
{
    return esp_sleep_get_ext1_wakeup_status();
}
