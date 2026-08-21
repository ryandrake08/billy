#include "audio.h"
#include "hal.h"
#include "board.h"
#include "fish_config.h"
#include "motors.h"
#include "activation.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "audio";

#define MIC_SAMPLE_RATE 16000       // mic capture / whisper rate
#define AMP_SAMPLE_RATE 24000       // amp output rate — fixed to match Kokoro TTS's native rate
#define TWO_PI          6.28318530718f

void audio_init(void)
{
    // Amp enable: the MAX98357A's SD_MODE must be driven high; low/floating leaves it muted.
    // Release any hold left over from a deep sleep the chip just woke from (audio_mute_for_sleep()
    // holds this low through sleep) -- level is already the desired 1 (unmuted), so this can't
    // glitch it.
    hal_gpio_init_output(BOARD_AMP_SD_MODE, true);
    hal_gpio_hold_disable(BOARD_AMP_SD_MODE);
    ESP_LOGI(TAG, "init: amp SD_MODE high (unmuted)");

    // Amp: I2S0 TX, 16-bit stereo, fixed at AMP_SAMPLE_RATE. Runs continuously; auto-clears to
    // silence between writes (see hal_i2s_tx_init()'s doc comment).
    hal_i2s_config_t tx_cfg = {
        .bclk_pin = BOARD_AMP_I2S_BCLK,
        .ws_pin   = BOARD_AMP_I2S_LRCLK,
        .dout_pin = BOARD_AMP_I2S_DIN,
        .din_pin  = HAL_GPIO_UNUSED,
        .sample_rate = AMP_SAMPLE_RATE,
        .bit_width   = 16,
        .stereo      = true,
    };
    hal_i2s_tx_init(&tx_cfg);
    ESP_LOGI(TAG, "init: amp I2S0 TX running @ %d Hz", AMP_SAMPLE_RATE);

    // Mic: I2S1 RX. 24-bit sample MSB-first, left-justified in a 32-bit slot, left channel only
    // (the mic's L/R select is tied to GND -> left slot). Enabled continuously.
    hal_i2s_config_t rx_cfg = {
        .bclk_pin = BOARD_MIC_I2S_SCK,
        .ws_pin   = BOARD_MIC_I2S_WS,
        .dout_pin = HAL_GPIO_UNUSED,
        .din_pin  = BOARD_MIC_I2S_SD,
        .sample_rate = MIC_SAMPLE_RATE,
        .bit_width   = 32,
        .stereo      = false,
        .left_slot_only = true,
    };
    hal_i2s_rx_init(&rx_cfg);
    ESP_LOGI(TAG, "init: mic I2S1 RX running @ %d Hz", MIC_SAMPLE_RATE);
}

void audio_mute_for_sleep(void)
{
    hal_gpio_set(BOARD_AMP_SD_MODE, false);
    hal_gpio_hold_enable(BOARD_AMP_SD_MODE);
}

esp_err_t audio_mic_read_raw(int32_t *buf, size_t buf_len_bytes, size_t *out_bytes_read)
{
    return hal_i2s_rx_read(buf, buf_len_bytes, out_bytes_read);
}

// --- Amp playback: the TX channel runs continuously (enabled once in audio_init()) and ----------
// auto-clears to silence between chunks, so playing is just writing samples.

// Chunk size shared by every amp TX path (amp_write_mono's own buffer and amp_tone's synthesis
// buffer) -- one DMA write's worth of frames at a time.
#define AMP_CHUNK_FRAMES 256

// Write mono samples to the stereo TX by duplicating each into L and R. `chunk_cb`, if non-NULL,
// is invoked with each chunk right before it's written -- used to drive mouth-sync PWM off the
// audio actually being played (see audio_play below). False aborts playback as a motor fault.
typedef bool (*amp_chunk_cb_t)(const int16_t *chunk, int n);

static esp_err_t amp_write_mono(const int16_t *mono, size_t count, amp_chunk_cb_t chunk_cb)
{
    int16_t stereo[AMP_CHUNK_FRAMES * 2];
    size_t i = 0;
    while (i < count)
    {
        int n = (count - i) > AMP_CHUNK_FRAMES ? AMP_CHUNK_FRAMES : (int) (count - i);
        if (chunk_cb && !chunk_cb(&mono[i], n))
        {
            return FISH_ERR_MOTOR_FAULT;
        }
        for (int k = 0; k < n; k++)
        {
            stereo[2 * k]     = mono[i + k];
            stereo[2 * k + 1] = mono[i + k];
        }
        size_t written = 0;
        esp_err_t err = hal_i2s_tx_write(stereo, (size_t) n * 2 * sizeof(int16_t), &written);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "amp: i2s write failed: %s — aborting playback", esp_err_to_name(err));
            return FISH_ERR_AUDIO_HW;
        }
        i += n;
    }
    return ESP_OK;
}

#define AMP_TONE_AMPLITUDE (0.25f * 32767.0f)   // ~-12 dBFS per oscillator

// Synthesize and play a sine tone at AMP_SAMPLE_RATE. Used by the self-test (long) and the prompt
// beep (short).
static void amp_tone(int freq_hz, int ms, const char *label)
{
    const float dphase = TWO_PI * freq_hz / AMP_SAMPLE_RATE;
    int16_t buf[AMP_CHUNK_FRAMES];
    float phase = 0.0f;
    int frames_left = AMP_SAMPLE_RATE * ms / 1000;

    ESP_LOGI(TAG, "amp: %s tone %d Hz / %d ms", label, freq_hz, ms);
    while (frames_left > 0)
    {
        int n = frames_left > AMP_CHUNK_FRAMES ? AMP_CHUNK_FRAMES : frames_left;
        for (int i = 0; i < n; i++)
        {
            buf[i] = (int16_t) (AMP_TONE_AMPLITUDE * sinf(phase));
            phase += dphase;
            if (phase >= TWO_PI) phase -= TWO_PI;
        }
        amp_write_mono(buf, n, NULL);
        frames_left -= n;
    }
}

// Two clashing frequencies summed together -- deliberately dissonant so it can't be mistaken for
// the single-pitch prompt/self-test tones. Used for backend-failure cues.
static void amp_dissonant_tone(int freq1_hz, int freq2_hz, int ms, const char *label)
{
    const float dphase1 = TWO_PI * freq1_hz / AMP_SAMPLE_RATE;
    const float dphase2 = TWO_PI * freq2_hz / AMP_SAMPLE_RATE;
    int16_t buf[AMP_CHUNK_FRAMES];
    float phase1 = 0.0f, phase2 = 0.0f;
    int frames_left = AMP_SAMPLE_RATE * ms / 1000;

    ESP_LOGI(TAG, "amp: %s tone %d+%d Hz / %d ms", label, freq1_hz, freq2_hz, ms);
    while (frames_left > 0)
    {
        int n = frames_left > AMP_CHUNK_FRAMES ? AMP_CHUNK_FRAMES : frames_left;
        for (int i = 0; i < n; i++)
        {
            buf[i] = (int16_t) (AMP_TONE_AMPLITUDE * (sinf(phase1) + sinf(phase2)));
            phase1 += dphase1; if (phase1 >= TWO_PI) phase1 -= TWO_PI;
            phase2 += dphase2; if (phase2 >= TWO_PI) phase2 -= TWO_PI;
        }
        amp_write_mono(buf, n, NULL);
        frames_left -= n;
    }
}

#define PROMPT_TONE_HZ 880   // "ready — start talking" beep
#define PROMPT_TONE_MS 150

#define ERROR_TONE_HZ_LOW  300
#define ERROR_TONE_HZ_HIGH 320   // tight interval against LOW -> audible clashing/beating
#define ERROR_TONE_MS      400

void audio_prompt_tone(void)
{
    amp_tone(PROMPT_TONE_HZ, PROMPT_TONE_MS, "prompt");
}

void audio_error_tone(void)
{
    amp_dissonant_tone(ERROR_TONE_HZ_LOW, ERROR_TONE_HZ_HIGH, ERROR_TONE_MS, "error");
}

// --- Mouth lip-sync: RMS envelope of the audio actually being played -> a 3-level mouth gate ----
// A one-pole envelope follower over each amp-write chunk (~10.7 ms @ 24 kHz): fast attack (mouth
// snaps to a new level on onset), slower release (holds a level briefly through short gaps
// instead of chattering on every one). The mechanism can't usefully track duty proportionally --
// a bench duty sweep found it barely moves below ~65% -- so this is a small number of
// discrete gates, not a continuous value, same reasoning as head/tail staying pure on/off. Mouth
// gets a third, MID level (unlike head/tail, which stay binary open/close -- there's no plausible
// use case for a partial head or tail gesture, but a partial mouth reads as quieter/plainer speech
// vs. a wide-open emphasis, which is worth the extra state): that same bench duty sweep
// found ~80% duty is a real, visually distinct partial deflection on this mechanism (not just a
// weaker copy of 100% -- "80% looks like a ceiling, but 100% is noticeably stronger"), so it's
// used here as MID rather than picked arbitrarily. Reference level,
// rates, and thresholds are runtime-tunable (fish_config.h) -- still first-pass
// estimates, worth tuning by ear once the toy's real mechanism (vs. bench motors) is in the loop.

typedef enum
{
    MOUTH_CLOSED,
    MOUTH_MID,
    MOUTH_OPEN,
} mouth_gate_t;

static float        s_mouth_envelope = 0.0f;
static mouth_gate_t s_mouth_gate = MOUTH_CLOSED;   // last-commanded gate, so steady runs of
                                      // chunks (~93/s during playback) don't re-write the PWM
                                      // duty every chunk for no change in output.

static bool mouth_track_chunk(const int16_t *chunk, int n)
{
    const fish_config_t *cfg = fish_config_get();

    int64_t sumsq = 0;
    for (int i = 0; i < n; i++) sumsq += (int32_t) chunk[i] * (int32_t) chunk[i];
    float rms = sqrtf((float) sumsq / n);

    float target = rms / cfg->mouth_env_ref;
    if (target > 1.0f) target = 1.0f;

    float rate = (target > s_mouth_envelope) ? cfg->mouth_env_attack : cfg->mouth_env_release;
    s_mouth_envelope += (target - s_mouth_envelope) * rate;

    mouth_gate_t want = MOUTH_CLOSED;
    if (s_mouth_envelope > cfg->mouth_open_threshold)      want = MOUTH_OPEN;
    else if (s_mouth_envelope > cfg->mouth_mid_threshold)  want = MOUTH_MID;

    if (want != s_mouth_gate)
    {
        uint8_t pct = 0;
        if (want == MOUTH_OPEN)     pct = 100;
        else if (want == MOUTH_MID) pct = cfg->mouth_mid_duty_pct;
        if (!motors_set_mouth_pct(pct))
        {
            return false;
        }
        s_mouth_gate = want;
    }
    return true;
}

// Ramp isn't needed on close -- silence between sentences would otherwise leave the mouth ajar
// until the next chunk arrives, so snap it shut and reset the follower for the next utterance.
static bool mouth_close(void)
{
    s_mouth_envelope = 0.0f;
    s_mouth_gate = MOUTH_CLOSED;
    return motors_set_mouth_pct(0);
}

// Linear-interpolation resample of `in` (in_count samples @ in_rate) into a freshly
// PSRAM-allocated buffer @ out_rate, written through *out_count. Cheap and good enough for the
// one thing that needs it -- playing a mic capture back on a TX clock fixed at AMP_SAMPLE_RATE --
// not a general-purpose/high-quality resampler. Returns NULL on allocation failure.
static int16_t *resample_linear(const int16_t *in, size_t in_count, int in_rate, int out_rate,
                                 size_t *out_count)
{
    size_t n_out = (size_t) ((uint64_t) in_count * out_rate / in_rate);
    int16_t *out = heap_caps_malloc(n_out * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!out)
    {
        return NULL;
    }

    float step = (float) in_rate / (float) out_rate;
    for (size_t i = 0; i < n_out; i++)
    {
        float  src_pos = (float) i * step;
        size_t idx     = (size_t) src_pos;
        float  frac    = src_pos - (float) idx;
        int16_t s0 = in[idx];
        int16_t s1 = (idx + 1 < in_count) ? in[idx + 1] : s0;
        out[i] = (int16_t) ((float) s0 + ((float) s1 - (float) s0) * frac);
    }
    *out_count = n_out;
    return out;
}

esp_err_t audio_play(const audio_buf_t *audio, bool move_mouth)
{
    if (!audio || !audio->samples || audio->count == 0)
    {
        return ESP_OK;
    }

    const int16_t *samples = audio->samples;
    size_t count = audio->count;
    int16_t *resampled = NULL;

    if (audio->sample_rate != AMP_SAMPLE_RATE)
    {
        // The TX clock is fixed at init time (see audio_init()) -- resample to it rather than
        // playing at the wrong pitch/speed. The normal TTS path is already at AMP_SAMPLE_RATE
        // (Kokoro's native rate) and never reaches here; this is for callers like repeat-mode
        // playback of a 16 kHz mic capture.
        resampled = resample_linear(samples, count, audio->sample_rate, AMP_SAMPLE_RATE, &count);
        if (!resampled)
        {
            ESP_LOGE(TAG, "play: resample allocation failed (%u samples)",
                     (unsigned) audio->count);
            return ESP_ERR_NO_MEM;
        }
        samples = resampled;
    }

    esp_err_t err;
    if (move_mouth)
    {
        if (!motors_enable())
        {
            err = FISH_ERR_MOTOR_FAULT;
        }
        else
        {
            err = amp_write_mono(samples, count, mouth_track_chunk);
        }
        if (!mouth_close() && err == ESP_OK)
        {
            err = FISH_ERR_MOTOR_FAULT;
        }
    }
    else
    {
        err = amp_write_mono(samples, count, NULL);
    }
    heap_caps_free(resampled);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "play: %u samples @ %d Hz%s", (unsigned) audio->count, audio->sample_rate,
                 resampled ? " (resampled)" : "");
    }
    return err;
}

// --- Mic capture with energy VAD -------------------------------------------------------------

// Block size is a mic-read granularity choice, not a tunable -- stays compile-time. The onset/
// silence/voiced/drain/cap thresholds are runtime-tunable (fish_config.h): bench floor
// ~2000, speech >7000 on the 24-bit scale, but housing acoustics/mic placement will likely shift
// this once the fish is in its final housing.
#define VAD_BLOCK_SAMPLES 320          // 20 ms @ 16 kHz
#define VAD_BLOCK_MS      (VAD_BLOCK_SAMPLES * 1000 / MIC_SAMPLE_RATE)

esp_err_t audio_capture_utterance(audio_buf_t *out)
{
    // The button that triggered this turn (BUTTON-mode activation, or a wakeword-mode manual
    // override) may still be physically held down when capture starts -- wait for release first
    // so that same press doesn't immediately read as an abort below.
    activation_wait_for_button_release();

    const fish_config_t *cfg = fish_config_get();
    const size_t max_samples = (size_t) MIC_SAMPLE_RATE * cfg->capture_max_ms / 1000;
    int16_t *pcm = heap_caps_malloc(max_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm)
    {
        ESP_LOGE(TAG, "capture: PSRAM alloc of %u samples failed", (unsigned) max_samples);
        out->samples = NULL;
        out->count = 0;
        out->sample_rate = MIC_SAMPLE_RATE;
        return ESP_ERR_NO_MEM;
    }

    int32_t raw[VAD_BLOCK_SAMPLES];

    // The mic (RX runs continuously) just recorded the prompt tone into its DMA buffer; reading it
    // would false-trigger the VAD. Discard ~vad_drain_ms so the beep is gone and the room settles.
    for (int drained = 0; drained < cfg->vad_drain_ms; drained += VAD_BLOCK_MS)
    {
        size_t br = 0;
        if (audio_mic_read_raw(raw, sizeof raw, &br) != ESP_OK)
            continue;
        if (activation_button_press_debounced())
        {
            ESP_LOGI(TAG, "listen: button press — aborting capture");
            heap_caps_free(pcm);
            out->samples = NULL;
            out->count = 0;
            out->sample_rate = MIC_SAMPLE_RATE;
            return ESP_OK;
        }
    }

    bool capturing = false;
    int silence_ms = 0;
    int voiced_ms = 0;
    size_t count = 0;

    ESP_LOGI(TAG, "listen: waiting for speech...");
    while (count < max_samples)
    {
        if (activation_button_press_debounced())
        {
            ESP_LOGI(TAG, "listen: button press — aborting capture (%d ms voiced so far)", voiced_ms);
            heap_caps_free(pcm);
            out->samples = NULL;
            out->count = 0;
            out->sample_rate = MIC_SAMPLE_RATE;
            return ESP_OK;
        }

        size_t bytes_read = 0;
        if (audio_mic_read_raw(raw, sizeof raw, &bytes_read) != ESP_OK)
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
            if (rms > cfg->vad_onset_rms)
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

        if (rms < cfg->vad_onset_rms)
        {
            silence_ms += VAD_BLOCK_MS;
            if (silence_ms >= cfg->vad_silence_ms) break;
        }
        else
        {
            silence_ms = 0;
            voiced_ms += VAD_BLOCK_MS;
        }
    }

    // A real utterance has a meaningful amount of voiced audio; anything less is a click/pop and
    // would only make whisper hallucinate.
    if (voiced_ms < cfg->vad_min_voiced_ms)
    {
        ESP_LOGI(TAG, "listen: only %d ms voiced — ignoring (no real speech)", voiced_ms);
        heap_caps_free(pcm);
        out->samples = NULL;
        out->count = 0;
        out->sample_rate = MIC_SAMPLE_RATE;
        return ESP_OK;
    }

    out->samples = pcm;
    out->count = count;
    out->sample_rate = MIC_SAMPLE_RATE;
    ESP_LOGI(TAG, "listen: captured %u samples (%u ms, %d voiced)",
             (unsigned) count, (unsigned) (count * 1000 / MIC_SAMPLE_RATE), voiced_ms);
    return ESP_OK;
}

// --- Self-test (bench tool) ------------------------------------------------------------------

static void mic_level_monitor(void)
{
    int32_t buf[512];
    ESP_LOGI(TAG, "mic: level monitor — tap or talk near the mic to see rms/peak move");
    for (;;)
    {
        size_t bytes_read = 0;
        if (audio_mic_read_raw(buf, sizeof buf, &bytes_read) != ESP_OK)
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

void audio_selftest(void)
{
    ESP_LOGI(TAG, "audio self-test — amp tone, then live mic level (reset to replay the tone)");
    amp_tone(440, 2000, "self-test");
    ESP_LOGI(TAG, "amp: tone done — DMA auto-clears to silence");
    mic_level_monitor();   // does not return
}
