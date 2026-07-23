#include "hal.h"
#include "board.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <math.h>
#include <stdint.h>

static const char *TAG = "hal";

#define AUDIO_SAMPLE_RATE 16000       // mic capture / whisper rate
#define TWO_PI            6.28318530718f

// I²S channel handles: I2S0 TX -> MAX98357A amp (playback), I2S1 RX <- INMP441/ICS-43434 mic.
// The mic (RX) is enabled continuously; the amp (TX) is enabled only during playback so an idle
// channel can't loop its last DMA buffer out the speaker.
static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;

void audio_buf_free(audio_buf_t *buf)
{
    if (buf && buf->samples)
    {
        heap_caps_free(buf->samples);
    }
    if (buf)
    {
        buf->samples = NULL;
        buf->count = 0;
    }
}

void fish_hal_init(void)
{
    // Amp enable: the MAX98357A's SD_MODE must be driven high; low/floating leaves it muted.
    gpio_config_t sd_gpio = {
        .pin_bit_mask = 1ULL << BOARD_AMP_SD_MODE,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&sd_gpio));
    gpio_set_level(BOARD_AMP_SD_MODE, 1);

    // Activation inputs: button (BOARD_BUTTON) and mode switch (BOARD_MODE_SW), both active-low
    // with internal pull-ups — a floating jumper reads high, grounding it reads low. (The button
    // still needs an EXTERNAL pull-up for the deep-sleep wake path in Stage 3.4; the internal pull
    // is enough for the bench polling used here.)
    gpio_config_t in_gpio = {
        .pin_bit_mask = (1ULL << BOARD_BUTTON) | (1ULL << BOARD_MODE_SW),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in_gpio));

    // Amp: I2S0 TX, 16-bit stereo. Left initialized-but-disabled; each playback enables the
    // channel and reconfigures the clock to the audio's own rate via amp_begin (16 kHz tones,
    // 24 kHz Kokoro TTS). So this clk_cfg rate is only a required placeholder for init — never
    // the effective output rate.
    i2s_chan_config_t tx_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan, &s_tx, NULL));
    i2s_std_config_t tx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_AMP_I2S_BCLK,
            .ws   = BOARD_AMP_I2S_LRCLK,
            .dout = BOARD_AMP_I2S_DIN,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &tx_std));

    // Mic: I2S1 RX. 24-bit sample MSB-first, left-justified in a 32-bit slot, left channel only
    // (the mic's L/R select is tied to GND -> left slot). Enabled continuously.
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

    ESP_LOGI(TAG, "init — amp SD_MODE high; I2S0 TX (amp) ready, I2S1 RX (mic) @ %d Hz",
             AUDIO_SAMPLE_RATE);
}

// --- Amp playback: enable -> write -> disable, so the amp is silent between chunks. -----------

// Start playback at `rate` Hz. TX must be disabled on entry (reconfig requires it); leaves it
// enabled for amp_write_mono().
static void amp_begin(int rate)
{
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(s_tx, &clk));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
}

// Write mono samples to the stereo TX by duplicating each into L and R.
static void amp_write_mono(const int16_t *mono, size_t count)
{
    int16_t stereo[256 * 2];
    size_t i = 0;
    while (i < count)
    {
        int n = (count - i) > 256 ? 256 : (int) (count - i);
        for (int k = 0; k < n; k++)
        {
            stereo[2 * k]     = mono[i + k];
            stereo[2 * k + 1] = mono[i + k];
        }
        size_t written = 0;
        i2s_channel_write(s_tx, stereo, (size_t) n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        i += n;
    }
}

static void amp_end(void)
{
    ESP_ERROR_CHECK(i2s_channel_disable(s_tx));
}

// Synthesize and play a sine tone. Used by the self-test (long) and the prompt beep (short).
static void amp_tone(int freq_hz, int ms, const char *label)
{
    const float amplitude = 0.25f * 32767.0f;   // ~-12 dBFS
    const float dphase = TWO_PI * freq_hz / AUDIO_SAMPLE_RATE;
    int16_t buf[256];
    float phase = 0.0f;
    int frames_left = AUDIO_SAMPLE_RATE * ms / 1000;

    ESP_LOGI(TAG, "amp: %s tone %d Hz / %d ms", label, freq_hz, ms);
    amp_begin(AUDIO_SAMPLE_RATE);
    while (frames_left > 0)
    {
        int n = frames_left > 256 ? 256 : frames_left;
        for (int i = 0; i < n; i++)
        {
            buf[i] = (int16_t) (amplitude * sinf(phase));
            phase += dphase;
            if (phase >= TWO_PI) phase -= TWO_PI;
        }
        amp_write_mono(buf, n);
        frames_left -= n;
    }
    amp_end();
}

void fish_hal_prompt_tone(void)
{
    amp_tone(880, 150, "prompt");
}

void fish_hal_play_with_mouth(const audio_buf_t *audio)
{
    if (!audio || !audio->samples || audio->count == 0)
    {
        return;
    }
    amp_begin(audio->sample_rate);
    amp_write_mono(audio->samples, audio->count);
    amp_end();
    ESP_LOGI(TAG, "play: %u samples @ %d Hz", (unsigned) audio->count, audio->sample_rate);
}

// --- Mic capture with energy VAD -------------------------------------------------------------

#define VAD_BLOCK_SAMPLES 320          // 20 ms @ 16 kHz
#define VAD_BLOCK_MS      (VAD_BLOCK_SAMPLES * 1000 / AUDIO_SAMPLE_RATE)
#define VAD_ONSET_RMS     6000         // 24-bit scale: idle floor ~2000, speech >7000
#define VAD_SILENCE_MS    600          // end the turn after this much sub-threshold audio
#define VAD_DRAIN_MS      250          // discard the mic's buffered prompt tone before listening
#define VAD_MIN_VOICED_MS 250          // reject a capture with less real speech than this (clicks)
#define CAPTURE_MAX_MS    10000        // hard cap on one utterance

void fish_hal_capture_utterance(audio_buf_t *out)
{
    const size_t max_samples = (size_t) AUDIO_SAMPLE_RATE * CAPTURE_MAX_MS / 1000;
    int16_t *pcm = heap_caps_malloc(max_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm)
    {
        ESP_LOGE(TAG, "capture: PSRAM alloc of %u samples failed", (unsigned) max_samples);
        out->samples = NULL;
        out->count = 0;
        out->sample_rate = AUDIO_SAMPLE_RATE;
        return;
    }

    int32_t raw[VAD_BLOCK_SAMPLES];

    // The mic (RX runs continuously) just recorded the prompt tone into its DMA buffer; reading it
    // would false-trigger the VAD. Discard ~VAD_DRAIN_MS so the beep is gone and the room settles.
    for (int drained = 0; drained < VAD_DRAIN_MS; drained += VAD_BLOCK_MS)
    {
        size_t br = 0;
        i2s_channel_read(s_rx, raw, sizeof raw, &br, portMAX_DELAY);
    }

    bool capturing = false;
    int silence_ms = 0;
    int voiced_ms = 0;
    size_t count = 0;

    ESP_LOGI(TAG, "listen: waiting for speech...");
    while (count < max_samples)
    {
        size_t bytes_read = 0;
        if (i2s_channel_read(s_rx, raw, sizeof raw, &bytes_read, portMAX_DELAY) != ESP_OK)
            continue;
        int n = (int) (bytes_read / sizeof(int32_t));
        if (n == 0) continue;

        // Block RMS on the 24-bit sample (>>8), matching the self-test's scale/thresholds.
        int64_t sum = 0;
        for (int i = 0; i < n; i++) sum += (raw[i] >> 8);
        int32_t mean = (int32_t) (sum / n);
        double sumsq = 0.0;
        for (int i = 0; i < n; i++)
        {
            int32_t s = (raw[i] >> 8) - mean;
            sumsq += (double) s * (double) s;
        }
        int32_t rms = (int32_t) sqrt(sumsq / n);

        if (!capturing)
        {
            if (rms > VAD_ONSET_RMS)
            {
                capturing = true;
                ESP_LOGI(TAG, "listen: onset (rms=%ld), capturing...", (long) rms);
            }
            else
            {
                continue;   // discard pre-speech silence
            }
        }

        // Store the block as 16-bit PCM (top 16 bits of the 24-bit sample).
        for (int i = 0; i < n && count < max_samples; i++)
        {
            pcm[count++] = (int16_t) (raw[i] >> 16);
        }

        if (rms < VAD_ONSET_RMS)
        {
            silence_ms += VAD_BLOCK_MS;
            if (silence_ms >= VAD_SILENCE_MS) break;
        }
        else
        {
            silence_ms = 0;
            voiced_ms += VAD_BLOCK_MS;
        }
    }

    // A real utterance has a meaningful amount of voiced audio; anything less is a click/pop and
    // would only make whisper hallucinate. Drop it and let the runloop listen again.
    if (voiced_ms < VAD_MIN_VOICED_MS)
    {
        ESP_LOGI(TAG, "listen: only %d ms voiced — ignoring (no real speech)", voiced_ms);
        heap_caps_free(pcm);
        out->samples = NULL;
        out->count = 0;
        out->sample_rate = AUDIO_SAMPLE_RATE;
        return;
    }

    out->samples = pcm;
    out->count = count;
    out->sample_rate = AUDIO_SAMPLE_RATE;
    ESP_LOGI(TAG, "listen: captured %u samples (%u ms, %d voiced)",
             (unsigned) count, (unsigned) (count * 1000 / AUDIO_SAMPLE_RATE), voiced_ms);
}

// --- Self-test (bench tool) ------------------------------------------------------------------

static void mic_level_monitor(void)
{
    int32_t buf[512];
    ESP_LOGI(TAG, "mic: level monitor — tap or talk near the mic to see rms/peak move");
    for (;;)
    {
        size_t bytes_read = 0;
        if (i2s_channel_read(s_rx, buf, sizeof buf, &bytes_read, portMAX_DELAY) != ESP_OK)
            continue;
        int n = (int) (bytes_read / sizeof(int32_t));
        if (n == 0) continue;

        int64_t sum = 0;
        for (int i = 0; i < n; i++) sum += (buf[i] >> 8);
        int32_t mean = (int32_t) (sum / n);
        double sumsq = 0.0;
        int32_t peak = 0;
        for (int i = 0; i < n; i++)
        {
            int32_t s = (buf[i] >> 8) - mean;
            int32_t mag = s < 0 ? -s : s;
            if (mag > peak) peak = mag;
            sumsq += (double) s * (double) s;
        }
        double rms = sqrt(sumsq / n);
        int dbfs = rms > 1.0 ? (int) (20.0 * log10(rms / 8388608.0)) : -120;
        ESP_LOGI(TAG, "mic: rms=%ld peak=%ld (~%d dBFS)", (long) rms, (long) peak, dbfs);
    }
}

void fish_hal_selftest(void)
{
    ESP_LOGI(TAG, "audio self-test — amp tone, then live mic level (reset to replay the tone)");
    amp_tone(440, 2000, "self-test");
    ESP_LOGI(TAG, "amp: tone done, TX stopped — speaker silent");
    mic_level_monitor();   // does not return
}

// --- Activation: mode switch + wake sources (button / wake word) ------------------------------
//
// Two activation modes, selected by the physical mode switch (BOARD_MODE_SW, §8.2):
//   BUTTON   — press-to-talk. The fish can deep-sleep and wake on the button GPIO for months of
//              standby. Lowest power; needs a deliberate press.
//   WAKEWORD — hands-free "hey billy". The CPU + mic stay powered to listen continuously, so this
//              mode cannot deep-sleep — that is the standby-power tradeoff (§8.2).
//
// The mode switch and button are bench-wired as bare jumpers on their GPIOs (grounding = "switch
// selected" / "button pressed"), so these reads are real. Only the wake-word detector is still a
// stub — it lands in the next steps (a built-in word first, then the trained "hey billy" model).

typedef enum
{
    WAKE_MODE_BUTTON,
    WAKE_MODE_WAKEWORD,
} wake_mode_t;

#define WAKE_POLL_MS         20    // poll interval for wake sources and the mode switch
#define BUTTON_DEBOUNCE_MS   40    // press must persist this long to count (contact bounce)
#define WAKEWORD_STUB_SIM_MS 3000  // stubbed detector's simulated time-to-detect

static const char *wake_mode_name(wake_mode_t m)
{
    return m == WAKE_MODE_BUTTON ? "BUTTON" : "WAKEWORD";
}

static wake_mode_t current_wake_mode(void)
{
    // BOARD_MODE_SW has an internal pull-up: floating (high) selects the low-power default,
    // BUTTON; grounding it selects hands-free WAKEWORD. Read fresh each turn so moving the jumper
    // takes effect immediately.
    return gpio_get_level(BOARD_MODE_SW) ? WAKE_MODE_BUTTON : WAKE_MODE_WAKEWORD;
}

// Active-low: the jumper grounded to GND reads 0 = pressed.
static bool button_pressed(void)
{
    return gpio_get_level(BOARD_BUTTON) == 0;
}

// BUTTON mode: block until a fresh, debounced press. If the jumper is already grounded from the
// previous turn, wait for release first so a held wire can't auto-advance every turn — each turn
// then needs a deliberate press edge. Returns true on a press; false if the mode switch moved off
// BUTTON, so the runloop can re-dispatch to the other wake source without waiting a whole turn.
static bool wait_for_button(void)
{
    ESP_LOGI(TAG, "wake(button): waiting for a press on GPIO %d (ground the jumper to press)",
             BOARD_BUTTON);
    while (button_pressed())
    {
        if (current_wake_mode() != WAKE_MODE_BUTTON) return false;
        vTaskDelay(pdMS_TO_TICKS(WAKE_POLL_MS));
    }
    for (;;)
    {
        if (current_wake_mode() != WAKE_MODE_BUTTON) return false;
        if (button_pressed())
        {
            vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
            if (button_pressed())
            {
                ESP_LOGI(TAG, "wake(button): press detected");
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(WAKE_POLL_MS));
    }
}

// WAKEWORD mode: block until "hey billy" is heard on the continuously-running mic. The detector
// lands in the next steps; stubbed for now (simulates a detection so the loop advances). Returns
// true on detection; false if the mode switch moved off WAKEWORD (mirrors wait_for_button).
static bool wait_for_wakeword(void)
{
    ESP_LOGI(TAG, "wake(wakeword): detector not integrated yet — simulating detection in %d ms",
             WAKEWORD_STUB_SIM_MS);
    for (int elapsed = 0; elapsed < WAKEWORD_STUB_SIM_MS; elapsed += WAKE_POLL_MS)
    {
        if (current_wake_mode() != WAKE_MODE_WAKEWORD) return false;
        vTaskDelay(pdMS_TO_TICKS(WAKE_POLL_MS));
    }
    ESP_LOGI(TAG, "wake(wakeword): detected (simulated)");
    return true;
}

void fish_hal_prepare_sleep(void)
{
    if (current_wake_mode() == WAKE_MODE_BUTTON)
    {
        // TODO (Stage 3.4 low-power): park motors (nSLEEP low), mute the amp, arm BOARD_BUTTON as
        // the wake source, and enter deep sleep here.
        ESP_LOGI(TAG, "prepare sleep (button mode): would park motors + arm button wake on GPIO %d",
                 BOARD_BUTTON);
    }
    else
    {
        // Hands-free: the mic must stay live for the wake-word detector, so we don't sleep.
        ESP_LOGI(TAG, "prepare sleep (wakeword mode): mic stays live for detection — no sleep");
    }
}

void fish_hal_wait_for_wake(void)
{
    // Re-dispatch whenever the mode switch flips mid-wait: the wait functions return false when
    // they see the mode change, so we just re-read and enter the other one. Only a real wake event
    // (true) returns to the runloop.
    for (;;)
    {
        wake_mode_t mode = current_wake_mode();
        ESP_LOGI(TAG, "wait for wake — mode=%s", wake_mode_name(mode));
        bool woke = (mode == WAKE_MODE_BUTTON) ? wait_for_button() : wait_for_wakeword();
        if (woke) return;
        ESP_LOGI(TAG, "wake: mode switch flipped — re-dispatching");
    }
}

// --- Motor choreography: still stubs. They log intent so the runloop can be exercised. --------

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
