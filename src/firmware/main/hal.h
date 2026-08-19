// True low-level HAL: generic primitives (GPIO, ADC1, LED strip, PWM, I2S, deep sleep),
// parameterized by pin/channel, no fish-specific naming and no board.h dependency of its own.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define HAL_GPIO_UNUSED (-1)   // pass for an I2S pin role that isn't wired (matches I2S_GPIO_UNUSED)

// --- GPIO ----------------------------------------------------------------------------------------

void hal_gpio_reset(int pin);   // detach from any peripheral (e.g. ADC) and return to default state
void hal_gpio_init_output(int pin, bool initial_level);
void hal_gpio_init_input(int pin, bool pull_up);
bool hal_gpio_get(int pin);
void hal_gpio_set(int pin, bool level);
void hal_gpio_hold_enable(int pin);    // survives deep sleep only with hal_deep_sleep_hold_enable() also set
void hal_gpio_hold_disable(int pin);

typedef void (*hal_gpio_interrupt_handler_t)(void *arg);  // GPIO interrupt handling callback
void hal_gpio_add_falling_interrupt(int pin, hal_gpio_interrupt_handler_t handler, void *arg);

// --- ADC1, shared by any single-shot analog read --------------------------------------------------
// Reconfigures the channel on every hal_adc_read() call rather than caching a pin->channel
// mapping -- these are infrequent, non-hot-path reads, so the bookkeeping to avoid that isn't
// worth it. Fixed at 12 dB attenuation (0-3.3V range), the only mode anything on this board uses.

void hal_adc_init(void);
int  hal_adc_read(int gpio_pin);

// --- Single WS2812 pixel over RMT ------------------------------------------------------------------

void hal_led_init(int gpio_pin);
void hal_led_show_rgb(uint8_t r, uint8_t g, uint8_t b);
void hal_led_clear(void);

// --- PWM (LEDC): one shared timer, N independent duty channels off it ------------------------------

void hal_pwm_init_timer(int freq_hz, int resolution_bits);
void hal_pwm_init_channel(int channel, int gpio_pin);
void hal_pwm_set_duty(int channel, uint32_t duty);   // raw duty, 0..(2^resolution_bits - 1); no clamping

// --- I2S: one TX (continuous, auto-clears to silence when idle) + one RX channel ------------------

typedef struct
{
    int bclk_pin, ws_pin, dout_pin, din_pin;   // HAL_GPIO_UNUSED for a role this channel doesn't use
    int  sample_rate;
    int  bit_width;        // 16 or 32
    bool stereo;            // false = mono
    bool left_slot_only;    // RX only: pin the slot mask to the left channel
} hal_i2s_config_t;

void hal_i2s_tx_init(const hal_i2s_config_t *cfg);
esp_err_t hal_i2s_tx_write(const void *data, size_t len, size_t *out_written);

void hal_i2s_rx_init(const hal_i2s_config_t *cfg);
esp_err_t hal_i2s_rx_read(void *buf, size_t len, size_t *out_read);

// --- Deep sleep ------------------------------------------------------------------------------------

void hal_deep_sleep_hold_enable(void);                        // global gpio_deep_sleep_hold_en()
void hal_deep_sleep_enable_ext1_wakeup(uint64_t pin_mask);     // ANY_LOW -- the only mode ever used
void hal_deep_sleep_enter(void);                               // esp_deep_sleep_start() -- never returns
bool hal_deep_sleep_woke_on_ext1(void);
uint64_t hal_deep_sleep_ext1_wake_pins(void);
