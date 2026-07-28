#include "hal.h"
#include "board.h"
#include "wakeword.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "led_strip.h"
#include <math.h>
#include <stdint.h>

static const char *TAG = "hal";

#define MIC_SAMPLE_RATE 16000       // mic capture / whisper rate
#define AMP_SAMPLE_RATE 24000       // amp output rate — fixed to match Kokoro TTS's native rate
#define TWO_PI          6.28318530718f

// I²S channel handles: I2S0 TX -> MAX98357A amp (playback), I2S1 RX <- INMP441/ICS-43434 mic.
// Both are enabled once in fish_hal_init() and run continuously for the device's uptime — simpler
// than enabling/disabling TX per chunk, and just as silent: auto_clear_after_cb (below) zeroes an
// idle channel's DMA buffers instead of looping stale content.
//
// (A prior investigation into audible noise at the start of playback suspected the TX enable/
// disable cycle, but that wasn't it — the actual cause was the amp sharing the ESP32 dev board's
// onboard 3.3V regulator with the CPU/WiFi; moving the amp to the board's 5V rail fixed it.)
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

// --- Status LED: single WS2812, driven over RMT via the led_strip component ------------------

static led_strip_handle_t s_status_led;

void fish_hal_status_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = BOARD_STATUS_LED,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,   // 10 MHz — standard RMT tick rate for WS2812 timing
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_status_led));
    led_strip_clear(s_status_led);
}

// Dim RGB triples, not 0-255 — a WS2812 at full brightness is eye-searing a few inches away on a
// bench. Indexed directly by fish_status_t.
static const uint8_t STATUS_COLORS[][3] = {
    [FISH_STATUS_BOOT]      = { 20, 20,  0 },   // yellow
    [FISH_STATUS_WIFI_WAIT] = { 15, 15, 15 },   // white
    [FISH_STATUS_IDLE]      = {  0, 20,  0 },   // green
    [FISH_STATUS_LISTEN]    = {  0,  0, 20 },   // blue
    [FISH_STATUS_THINK]     = {  0, 20, 20 },   // cyan
    [FISH_STATUS_SPEAK]     = { 15,  0, 20 },   // violet
    [FISH_STATUS_ERROR]     = { 20,  0,  0 },   // red
};

void fish_hal_set_status(fish_status_t status)
{
    const uint8_t *c = STATUS_COLORS[status];
    led_strip_set_pixel(s_status_led, 0, c[0], c[1], c[2]);
    led_strip_refresh(s_status_led);
}

// --- Motor drivers: 2x DRV8833, three unidirectional channels (mouth/head/tail) ----------------
// Each motor is IN1 = LEDC PWM, IN2 held low (spring-return; board.h). nSLEEP gates both chips
// together (high = enabled, low = ~uA parked); nFAULT is both chips' open-drain fault line,
// wire-OR'd onto one input (low = OCP/thermal/UVLO on either chip).

#define MOTOR_PWM_FREQ_HZ     20000              // above audible range -- avoids motor whine
#define MOTOR_PWM_RESOLUTION  LEDC_TIMER_10_BIT
#define MOTOR_DUTY_MAX        ((1 << MOTOR_PWM_RESOLUTION) - 1)   // ledc_timer_bit_t value == bit width
#define MOTOR_PWM_TIMER       LEDC_TIMER_0
#define MOTOR_PWM_MODE        LEDC_LOW_SPEED_MODE  // esp32s3 has no high-speed LEDC mode

typedef enum
{
    MOTOR_MOUTH = LEDC_CHANNEL_0,
    MOTOR_HEAD  = LEDC_CHANNEL_1,
    MOTOR_TAIL  = LEDC_CHANNEL_2,
} motor_channel_t;

static void motor_enable(void)
{
    gpio_set_level(BOARD_DRV_NSLEEP, 1);
}

static void motor_disable(void)
{
    gpio_set_level(BOARD_DRV_NSLEEP, 0);
}

static bool motor_fault_active(void)
{
    return gpio_get_level(BOARD_DRV_NFAULT) == 0;
}

static void motor_set_duty(motor_channel_t ch, uint32_t duty)
{
    if (duty > MOTOR_DUTY_MAX) duty = MOTOR_DUTY_MAX;
    ledc_set_duty(MOTOR_PWM_MODE, (ledc_channel_t) ch, duty);
    ledc_update_duty(MOTOR_PWM_MODE, (ledc_channel_t) ch);
}

void fish_hal_init(void)
{
    // Motors: IN2 pins are plain GPIO outputs, held low -- each motor is unidirectional (spring
    // return).
    gpio_config_t in2_gpio = {
        .pin_bit_mask = (1ULL << BOARD_MOUTH_IN2) | (1ULL << BOARD_HEAD_IN2) | (1ULL << BOARD_TAIL_IN2),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&in2_gpio));
    gpio_set_level(BOARD_MOUTH_IN2, 0);
    gpio_set_level(BOARD_HEAD_IN2, 0);
    gpio_set_level(BOARD_TAIL_IN2, 0);

    // nSLEEP: output, starts low -- motors stay parked until the first drive call enables it.
    gpio_config_t nsleep_gpio = {
        .pin_bit_mask = 1ULL << BOARD_DRV_NSLEEP,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&nsleep_gpio));
    gpio_set_level(BOARD_DRV_NSLEEP, 0);

    // nFAULT: open-drain from both chips, wire-OR'd. External pull-up is mandatory (SCOPING.md
    // §4.1); the internal pull is harmless alongside it.
    gpio_config_t nfault_gpio = {
        .pin_bit_mask = 1ULL << BOARD_DRV_NFAULT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&nfault_gpio));

    // One LEDC timer shared by all three motor channels (same frequency/resolution).
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = MOTOR_PWM_MODE,
        .duty_resolution = MOTOR_PWM_RESOLUTION,
        .timer_num       = MOTOR_PWM_TIMER,
        .freq_hz         = MOTOR_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    const struct { motor_channel_t ch; int gpio; } motor_channels[] = {
        { MOTOR_MOUTH, BOARD_MOUTH_IN1 },
        { MOTOR_HEAD,  BOARD_HEAD_IN1 },
        { MOTOR_TAIL,  BOARD_TAIL_IN1 },
    };
    for (size_t i = 0; i < sizeof(motor_channels) / sizeof(motor_channels[0]); i++)
    {
        ledc_channel_config_t ch_cfg = {
            .gpio_num   = motor_channels[i].gpio,
            .speed_mode = MOTOR_PWM_MODE,
            .channel    = motor_channels[i].ch,
            .timer_sel  = MOTOR_PWM_TIMER,
            .duty       = 0,
            .hpoint     = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));
    }

    // Amp enable: the MAX98357A's SD_MODE must be driven high; low/floating leaves it muted.
    gpio_config_t sd_gpio = {
        .pin_bit_mask = 1ULL << BOARD_AMP_SD_MODE,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&sd_gpio));
    gpio_set_level(BOARD_AMP_SD_MODE, 1);

    // Activation inputs: button (BOARD_BUTTON) and mode switch (BOARD_MODE_SW), both active-low
    // with internal pull-ups — a floating jumper reads high, grounding it reads low. (The button
    // still needs an EXTERNAL pull-up for the deep-sleep wake path (not yet implemented); the
    // internal pull is enough for the bench polling used here.)
    gpio_config_t in_gpio = {
        .pin_bit_mask = (1ULL << BOARD_BUTTON) | (1ULL << BOARD_MODE_SW),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in_gpio));

    // Amp: I2S0 TX, 16-bit stereo, fixed at AMP_SAMPLE_RATE. auto_clear_after_cb makes the
    // hardware zero each DMA buffer once it's sent and nothing new has replaced it, so the amp
    // plays silence whenever amp_write_mono() isn't actively feeding it — without ever needing to
    // stop the clock (see the top-of-file note on why that mattered).
    i2s_chan_config_t tx_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    tx_chan.auto_clear_after_cb = true;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan, &s_tx, NULL));
    i2s_std_config_t tx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AMP_SAMPLE_RATE),
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
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));

    // Mic: I2S1 RX. 24-bit sample MSB-first, left-justified in a 32-bit slot, left channel only
    // (the mic's L/R select is tied to GND -> left slot). Enabled continuously.
    i2s_chan_config_t rx_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan, NULL, &s_rx));
    i2s_std_config_t rx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
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

    ESP_LOGI(TAG, "init — motors: 3x LEDC PWM @ %d Hz configured, nSLEEP low (parked); "
                  "amp SD_MODE high; I2S0 TX (amp) running @ %d Hz, I2S1 RX (mic) running @ %d Hz",
             MOTOR_PWM_FREQ_HZ, AMP_SAMPLE_RATE, MIC_SAMPLE_RATE);
}

// --- Amp playback: the TX channel runs continuously (enabled once in fish_hal_init()) and -------
// auto-clears to silence between chunks, so playing is just writing samples.

// Write mono samples to the stereo TX by duplicating each into L and R. `chunk_cb`, if non-NULL,
// is invoked with each chunk right before it's written -- used to drive mouth-sync PWM off the
// audio actually being played (see fish_hal_play_with_mouth below).
typedef void (*amp_chunk_cb_t)(const int16_t *chunk, int n);

static void amp_write_mono(const int16_t *mono, size_t count, amp_chunk_cb_t chunk_cb)
{
    int16_t stereo[256 * 2];
    size_t i = 0;
    while (i < count)
    {
        int n = (count - i) > 256 ? 256 : (int) (count - i);
        if (chunk_cb) chunk_cb(&mono[i], n);
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

// Synthesize and play a sine tone at AMP_SAMPLE_RATE. Used by the self-test (long) and the prompt
// beep (short).
static void amp_tone(int freq_hz, int ms, const char *label)
{
    const float amplitude = 0.25f * 32767.0f;   // ~-12 dBFS
    const float dphase = TWO_PI * freq_hz / AMP_SAMPLE_RATE;
    int16_t buf[256];
    float phase = 0.0f;
    int frames_left = AMP_SAMPLE_RATE * ms / 1000;

    ESP_LOGI(TAG, "amp: %s tone %d Hz / %d ms", label, freq_hz, ms);
    while (frames_left > 0)
    {
        int n = frames_left > 256 ? 256 : frames_left;
        for (int i = 0; i < n; i++)
        {
            buf[i] = (int16_t) (amplitude * sinf(phase));
            phase += dphase;
            if (phase >= TWO_PI) phase -= TWO_PI;
        }
        amp_write_mono(buf, n, NULL);
        frames_left -= n;
    }
}

void fish_hal_prompt_tone(void)
{
    amp_tone(880, 150, "prompt");
}

// --- Mouth lip-sync: RMS envelope of the audio actually being played -> mouth open/close --------
// A one-pole envelope follower over each amp-write chunk (~10.7 ms @ 24 kHz): fast attack (mouth
// snaps open on onset), slower release (holds open briefly through short gaps instead of
// chattering on every one). The mechanism can't usefully track duty proportionally -- bench
// testing found it barely moves below ~65% and needs ~100% for a full stroke (WIRING.md §6.1) --
// so the smoothed envelope is used as a gate, not a proportional value: full duty above the
// threshold, off below it. Reference level, rates, and threshold are a first-pass estimate --
// tune on the bench, same as the VAD constants above.
#define MOUTH_ENV_REF       6000.0f   // RMS that saturates the envelope at 1.0 (16-bit PCM scale)
#define MOUTH_ENV_ATTACK    0.6f
#define MOUTH_ENV_RELEASE   0.15f
#define MOUTH_OPEN_THRESHOLD 0.3f     // envelope above this -> mouth snaps open; below -> closed

static float s_mouth_envelope = 0.0f;
static bool  s_mouth_open = false;   // last-commanded gate state, so steady open/closed runs of
                                      // chunks (~93/s during playback) don't re-write the LEDC
                                      // duty register every chunk for no change in output.

static void mouth_track_chunk(const int16_t *chunk, int n)
{
    int64_t sumsq = 0;
    for (int i = 0; i < n; i++) sumsq += (int32_t) chunk[i] * (int32_t) chunk[i];
    float rms = sqrtf((float) sumsq / n);

    float target = rms / MOUTH_ENV_REF;
    if (target > 1.0f) target = 1.0f;

    float rate = (target > s_mouth_envelope) ? MOUTH_ENV_ATTACK : MOUTH_ENV_RELEASE;
    s_mouth_envelope += (target - s_mouth_envelope) * rate;

    bool want_open = s_mouth_envelope > MOUTH_OPEN_THRESHOLD;
    if (want_open != s_mouth_open)
    {
        motor_set_duty(MOTOR_MOUTH, want_open ? MOTOR_DUTY_MAX : 0);
        s_mouth_open = want_open;
    }
}

// Ramp isn't needed on close -- silence between sentences would otherwise leave the mouth ajar
// until the next chunk arrives, so snap it shut and reset the follower for the next utterance.
static void mouth_close(void)
{
    s_mouth_envelope = 0.0f;
    s_mouth_open = false;
    motor_set_duty(MOTOR_MOUTH, 0);
}

void fish_hal_play_with_mouth(const audio_buf_t *audio)
{
    if (!audio || !audio->samples || audio->count == 0)
    {
        return;
    }
    if (audio->sample_rate != AMP_SAMPLE_RATE)
    {
        // The TX clock is fixed at init time (see fish_hal_init()) — a mismatched rate would play
        // at the wrong pitch/speed rather than getting resampled.
        ESP_LOGW(TAG, "play: audio is %d Hz but amp is fixed at %d Hz — will play at the wrong "
                      "speed", audio->sample_rate, AMP_SAMPLE_RATE);
    }
    motor_enable();
    amp_write_mono(audio->samples, audio->count, mouth_track_chunk);
    mouth_close();
    ESP_LOGI(TAG, "play: %u samples @ %d Hz", (unsigned) audio->count, audio->sample_rate);
}

// --- Mic capture with energy VAD -------------------------------------------------------------

#define VAD_BLOCK_SAMPLES 320          // 20 ms @ 16 kHz
#define VAD_BLOCK_MS      (VAD_BLOCK_SAMPLES * 1000 / MIC_SAMPLE_RATE)
#define VAD_ONSET_RMS     6000         // 24-bit scale: idle floor ~2000, speech >7000
#define VAD_SILENCE_MS    600          // end the turn after this much sub-threshold audio
#define VAD_DRAIN_MS      250          // discard the mic's buffered prompt tone before listening
#define VAD_MIN_VOICED_MS 250          // reject a capture with less real speech than this (clicks)
#define CAPTURE_MAX_MS    10000        // hard cap on one utterance

void fish_hal_capture_utterance(audio_buf_t *out)
{
    const size_t max_samples = (size_t) MIC_SAMPLE_RATE * CAPTURE_MAX_MS / 1000;
    int16_t *pcm = heap_caps_malloc(max_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm)
    {
        ESP_LOGE(TAG, "capture: PSRAM alloc of %u samples failed", (unsigned) max_samples);
        out->samples = NULL;
        out->count = 0;
        out->sample_rate = MIC_SAMPLE_RATE;
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
        out->sample_rate = MIC_SAMPLE_RATE;
        return;
    }

    out->samples = pcm;
    out->count = count;
    out->sample_rate = MIC_SAMPLE_RATE;
    ESP_LOGI(TAG, "listen: captured %u samples (%u ms, %d voiced)",
             (unsigned) count, (unsigned) (count * 1000 / MIC_SAMPLE_RATE), voiced_ms);
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
    ESP_LOGI(TAG, "amp: tone done — DMA auto-clears to silence");
    mic_level_monitor();   // does not return
}

// Fault-isolating: one motor at a time, sweeping duty from low to full so you can find the
// minimum duty that actually overcomes the mechanism's spring preload/gearing -- stops
// immediately on any fault rather than moving on to the next motor. No forced stall -- the
// motor shafts are friction-fit to their gears, so a deliberate stall risks the gears more than
// it proves the driver's OCP works (WIRING.md §6.2). Watch the motor itself while this runs; the
// firmware has no current sense, only nFAULT. NOTE: the duty->motion
// threshold tracks the motor rail's actual voltage (no buck -- WIRING.md §0), so a sweep run on a
// sagged battery will read higher thresholds than the same sweep on a fresh one.
void fish_hal_motor_selftest(void)
{
    ESP_LOGI(TAG, "motor self-test — one motor at a time, sweeping duty to find the motion threshold");

    if (motor_fault_active())
    {
        ESP_LOGE(TAG, "motor self-test: nFAULT already low before nSLEEP — check the fault line/pull-up before proceeding");
        return;
    }

    motor_enable();
    vTaskDelay(pdMS_TO_TICKS(20));   // let both DRV8833s settle out of sleep before reading nFAULT
    if (motor_fault_active())
    {
        ESP_LOGE(TAG, "motor self-test: nFAULT low right after nSLEEP high, at 0%% duty — check the motor rail before proceeding");
        motor_disable();
        return;
    }

    const struct { motor_channel_t ch; const char *name; } motors[] = {
        { MOTOR_MOUTH, "mouth" },
        { MOTOR_HEAD,  "head" },
        { MOTOR_TAIL,  "tail" },
    };
    const uint8_t sweep_pct[] = { 20, 35, 50, 65, 80, 100 };

    for (size_t i = 0; i < sizeof(motors) / sizeof(motors[0]); i++)
    {
        const char *name = motors[i].name;
        motor_channel_t ch = motors[i].ch;

        for (size_t s = 0; s < sizeof(sweep_pct) / sizeof(sweep_pct[0]); s++)
        {
            uint32_t duty = (MOTOR_DUTY_MAX * sweep_pct[s]) / 100;
            ESP_LOGI(TAG, "motor self-test: %s — %u%% duty, 2 s", name, (unsigned) sweep_pct[s]);
            motor_set_duty(ch, duty);
            vTaskDelay(pdMS_TO_TICKS(2000));
            motor_set_duty(ch, 0);
            if (motor_fault_active())
            {
                ESP_LOGE(TAG, "motor self-test: %s tripped nFAULT at %u%% duty — stopping here", name, (unsigned) sweep_pct[s]);
                motor_disable();
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(800));   // stopped, for a clean before/after contrast with the next step
        }

        vTaskDelay(pdMS_TO_TICKS(1500));   // extra pause before moving on to the next motor
    }

    motor_disable();
    ESP_LOGI(TAG, "motor self-test: done, all three motors clean — nSLEEP low (parked)");
}

// --- Activation: mode switch + wake sources (button / wake word) ------------------------------
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

typedef enum
{
    WAKE_MODE_BUTTON,
    WAKE_MODE_WAKEWORD,
} wake_mode_t;

#define WAKE_POLL_MS         20    // poll interval for the mode switch (button mode) / button debounce
#define BUTTON_DEBOUNCE_MS   40    // press must persist this long to count (contact bounce)

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

static bool button_mode_active(void)   { return current_wake_mode() == WAKE_MODE_BUTTON; }
static bool wakeword_mode_active(void) { return current_wake_mode() == WAKE_MODE_WAKEWORD; }

// Block until the button reads released, polling `keep_waiting` so a caller can bail out (e.g. on
// a mode-switch flip) instead of blocking indefinitely. This is what stops a held/stuck jumper
// from immediately counting as a fresh press when a wait function starts. Returns false if
// keep_waiting() ever returns false; true once released (or if it was never pressed).
static bool wait_for_button_release(bool (*keep_waiting)(void))
{
    while (button_pressed())
    {
        if (!keep_waiting()) return false;
        vTaskDelay(pdMS_TO_TICKS(WAKE_POLL_MS));
    }
    return true;
}

// Single-shot debounced press check: true only if the button is pressed now AND still pressed
// after BUTTON_DEBOUNCE_MS (contact bounce filtering). Cheap to poll once per loop iteration --
// only blocks for the debounce window when a press is actually seen.
static bool button_press_debounced(void)
{
    if (!button_pressed()) return false;
    vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
    return button_pressed();
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
        if (button_press_debounced())
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
// a time and feeds it to the detector; each step also checks the mode switch and button, so a
// flip or press preempts within one step. Returns true on detection/press; false if the mode
// switch moved off WAKEWORD.
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

        if (button_press_debounced())
        {
            ESP_LOGI(TAG, "wake(wakeword): button press detected (manual override)");
            return true;
        }

        size_t bytes_read = 0;
        if (i2s_channel_read(s_rx, raw, sizeof raw, &bytes_read, portMAX_DELAY) != ESP_OK)
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

void fish_hal_prepare_sleep(void)
{
    motor_disable();   // nSLEEP low -- park both DRV8833s (~uA) regardless of wake mode

    if (current_wake_mode() == WAKE_MODE_BUTTON)
    {
        // TODO (low-power, not yet implemented): mute the amp, arm BOARD_BUTTON as the wake
        // source, and enter deep sleep here.
        ESP_LOGI(TAG, "prepare sleep (button mode): motors parked; would arm button wake on GPIO %d",
                 BOARD_BUTTON);
    }
    else
    {
        // Hands-free: the mic must stay live for the wake-word detector, so we don't sleep.
        ESP_LOGI(TAG, "prepare sleep (wakeword mode): motors parked; mic stays live for detection — no sleep");
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

// --- Motor choreography: tail flap + head raise/relax. Mouth PWM is driven separately, off the
// playback envelope (see mouth_track_chunk above). Approx timings from SCOPING.md §4/§6.

#define TAIL_FLAP_MS 250   // one flap: drive out, then let the spring return it

static void motor_warn_if_fault(const char *what)
{
    if (motor_fault_active())
    {
        ESP_LOGW(TAG, "%s: nFAULT low — possible stall/OCP", what);
    }
}

// ACTIVATE: a single "I'm listening" gesture -- drive the tail out and let the spring return it.
// Blocking is fine here; it's a brief one-shot before LISTEN starts capturing.
void fish_hal_tail_flap(void)
{
    ESP_LOGI(TAG, "tail flap — 'I'm listening'");
    motor_enable();
    motor_set_duty(MOTOR_TAIL, MOTOR_DUTY_MAX);
    vTaskDelay(pdMS_TO_TICKS(TAIL_FLAP_MS));
    motor_set_duty(MOTOR_TAIL, 0);
    motor_warn_if_fault("tail flap");
}

// SPEAK: raise the head and hold it (non-blocking -- the spring-return motor stays driven for as
// long as fish_hal_head_relax() is withheld, which the runloop does until the whole reply is
// done, not just on audio silence -- a documented BillAI bug otherwise, SCOPING.md §6).
void fish_hal_head_out(void)
{
    ESP_LOGI(TAG, "head out — 'I'm talking'");
    motor_enable();
    motor_set_duty(MOTOR_HEAD, MOTOR_DUTY_MAX);
}

void fish_hal_head_relax(void)
{
    ESP_LOGI(TAG, "head relax");
    motor_set_duty(MOTOR_HEAD, 0);
    motor_warn_if_fault("head");
}
