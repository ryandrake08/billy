#include "hal.h"
#include "board.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include <math.h>
#include <stdint.h>

static const char *TAG = "hal";

#define AUDIO_SAMPLE_RATE 16000   // matches the whisper/Kokoro pipeline

// I²S channel handles: I2S0 TX -> MAX98357A amp (playback), I2S1 RX <- INMP441/ICS-43434 mic.
static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;

void fish_hal_init(void)
{
    // Amp enable: the MAX98357A's SD_MODE must be driven high; low/floating leaves it muted
    // (a "dead amp" that's actually just off).
    gpio_config_t sd_gpio = {
        .pin_bit_mask = 1ULL << BOARD_AMP_SD_MODE,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&sd_gpio));
    gpio_set_level(BOARD_AMP_SD_MODE, 1);

    // Amp: I2S0 TX, 16-bit stereo. The sine goes to both slots; the MAX98357A sums L+R to mono.
    i2s_chan_config_t tx_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan, &s_tx, NULL));
    i2s_std_config_t tx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,   // MAX98357A needs no MCLK
            .bclk = BOARD_AMP_I2S_BCLK,
            .ws   = BOARD_AMP_I2S_LRCLK,
            .dout = BOARD_AMP_I2S_DIN,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &tx_std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));

    // Mic: I2S1 RX. The INMP441/ICS-43434 clocks a 24-bit sample MSB-first, left-justified in a
    // 32-bit slot, in the left channel only (its L/R select is tied to GND -> left slot).
    i2s_chan_config_t rx_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan, NULL, &s_rx));
    i2s_std_config_t rx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_MIC_I2S_SCK,
            .ws   = BOARD_MIC_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = BOARD_MIC_I2S_SD,
        },
    };
    rx_std.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &rx_std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));

    ESP_LOGI(TAG, "init — amp SD_MODE high; I2S0 TX (amp) + I2S1 RX (mic) enabled @ %d Hz",
             AUDIO_SAMPLE_RATE);
}

// Play a sine tone on the amp for `ms` milliseconds. I²S TX has no back-channel, so this is the
// only real confirmation the amp is wired and unmuted: you listen for a clean tone.
static void play_test_tone(int freq_hz, int ms)
{
    static const float TWO_PI = 6.28318530718f;
    const float amplitude = 0.25f * 32767.0f;   // ~-12 dBFS: audible without blasting
    const float dphase = TWO_PI * freq_hz / AUDIO_SAMPLE_RATE;
    int16_t chunk[256 * 2];                      // 256 stereo frames
    const int frames_per_chunk = 256;
    int frames_left = AUDIO_SAMPLE_RATE * ms / 1000;
    float phase = 0.0f;

    ESP_LOGI(TAG, "amp: playing %d Hz tone for %d ms — listen for a clean tone", freq_hz, ms);
    while (frames_left > 0)
    {
        int n = frames_left < frames_per_chunk ? frames_left : frames_per_chunk;
        for (int i = 0; i < n; i++)
        {
            int16_t s = (int16_t) (amplitude * sinf(phase));
            phase += dphase;
            if (phase >= TWO_PI) phase -= TWO_PI;
            chunk[2 * i]     = s;   // left
            chunk[2 * i + 1] = s;   // right
        }
        size_t written = 0;
        i2s_channel_write(s_tx, chunk, (size_t) n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        frames_left -= n;
    }
}

// Continuously read the mic and log its level. Tap or talk near the mic and rms/peak should jump.
// A flat reading near zero (or a stuck constant) means the mic isn't delivering data — check
// power, the SD/SCK/WS wiring, and that the L/R select is tied to GND. Does not return.
static void mic_level_monitor(void)
{
    int32_t buf[512];
    ESP_LOGI(TAG, "mic: level monitor — tap or talk near the mic to see rms/peak move");
    for (;;)
    {
        size_t bytes_read = 0;
        if (i2s_channel_read(s_rx, buf, sizeof(buf), &bytes_read, portMAX_DELAY) != ESP_OK)
            continue;
        int n = (int) (bytes_read / sizeof(int32_t));
        if (n == 0) continue;

        // The 24-bit sample sits in the top bits of the 32-bit slot -> arithmetic shift down.
        int64_t sum = 0;
        for (int i = 0; i < n; i++) sum += (buf[i] >> 8);
        int32_t mean = (int32_t) (sum / n);

        double sumsq = 0.0;
        int32_t peak = 0;
        for (int i = 0; i < n; i++)
        {
            int32_t s = (buf[i] >> 8) - mean;   // remove the mic's DC bias
            int32_t mag = s < 0 ? -s : s;
            if (mag > peak) peak = mag;
            sumsq += (double) s * (double) s;
        }
        double rms = sqrt(sumsq / n);
        // Full scale for a 24-bit sample is 2^23; log an integer dBFS so no float printf is needed.
        int dbfs = rms > 1.0 ? (int) (20.0 * log10(rms / 8388608.0)) : -120;
        ESP_LOGI(TAG, "mic: rms=%ld peak=%ld (~%d dBFS)", (long) rms, (long) peak, dbfs);
    }
}

void fish_hal_selftest(void)
{
    ESP_LOGI(TAG, "audio self-test — amp tone, then live mic level (reset to replay the tone)");
    play_test_tone(440, 2000);
    // Stop the amp before monitoring the mic. An idle-but-enabled TX channel loops its last DMA
    // buffer, replaying the tone with a click at each ~90 ms wrap; disabling halts the I²S clock
    // so the MAX98357A goes silent and the mic can be exercised in quiet.
    ESP_ERROR_CHECK(i2s_channel_disable(s_tx));
    ESP_LOGI(TAG, "amp: tone done, TX stopped — speaker silent");
    mic_level_monitor();   // does not return
}

// --- Motor / wake choreography: still stubs until Stage 3.2/3.3. They log intent so the runloop
// can be exercised on-target now. ---

void fish_hal_prepare_sleep(void)
{
    ESP_LOGI(TAG, "prepare sleep — both DRV8833s nSLEEP low, amp muted, arm button wake on GPIO %d",
             BOARD_BUTTON);
}

void fish_hal_wait_for_wake(void)
{
    ESP_LOGI(TAG, "waiting for wake (stub returns immediately)");
}

void fish_hal_tail_flap(void)
{
    ESP_LOGI(TAG, "tail flap — 'I'm listening'");
}

void fish_hal_head_out(void)
{
    ESP_LOGI(TAG, "head out — 'I'm talking'");
}

void fish_hal_head_relax(void)
{
    ESP_LOGI(TAG, "head relax");
}

void fish_hal_capture_utterance(audio_buf_t *out)
{
    ESP_LOGI(TAG, "capture mic until VAD end-of-speech (stub)");
    out->len = 0;
}

void fish_hal_play_with_mouth(const audio_buf_t *audio)
{
    ESP_LOGI(TAG, "play %u samples + mouth PWM from RMS envelope (stub)", (unsigned) audio->len);
}
