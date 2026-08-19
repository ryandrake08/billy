// Audio I/O: I²S mic (energy-VAD capture) + I²S amp (tone/playback), and the mouth lip-sync
// envelope follower.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// A block of mono 16-bit PCM. `samples` is heap_caps-allocated in PSRAM; free with
// heap_caps_free(). A zeroed buffer (samples == NULL, count == 0) means "no audio".
typedef struct
{
    int16_t *samples;
    size_t   count;        // number of samples
    int      sample_rate;  // Hz
} audio_buf_t;

// Signals a local audio-hardware fault (currently: the amp TX write failing), as distinct from
// any ordinary ESP-IDF esp_err_t a network/backend call might also return. 0x8001 sits well
// outside the range of any ESP-IDF or driver error code in use here.
#define FISH_ERR_AUDIO_HW ((esp_err_t) 0x8001)

// A motor nFAULT interrupted mouth animation during playback.
#define FISH_ERR_MOTOR_FAULT ((esp_err_t) 0x8002)

// Amp SD_MODE high (unmuted) + both I²S controllers running continuously (mic RX, amp TX). Call
// once, before any other audio_* function.
void audio_init(void);

// BUTTON-mode deep sleep only: mutes the amp (SD_MODE low) and holds that level through the
// sleep.
void audio_mute_for_sleep(void);

// Raw mic RX read, one I2S read's worth of 32-bit slots (24-bit sample MSB-first, left channel
// only)
esp_err_t audio_mic_read_raw(int32_t *buf, size_t buf_len_bytes, size_t *out_bytes_read);

// A short "ready — start talking" beep on the amp.
void audio_prompt_tone(void);

// A brief dissonant alert on the amp -- two clashing frequencies, distinct from the mellow
// prompt/self-test tones -- for a backend/network failure (STT or the shim brain-hop).
void audio_error_tone(void);

// Bench audio self-test (not in the E2E boot path): 440 Hz tone, then a live mic-level log.
void audio_selftest(void);

// LISTEN: energy-VAD capture — wait for speech onset, capture until ~0.8 s of silence (or a hard
// cap). Also interruptible: a button press (activation.h) at any point aborts and returns the
// same "no audio" result as below. Allocates out->samples in PSRAM; the caller frees it with
// heap_caps_free(). Returns ESP_ERR_NO_MEM if the PSRAM allocation fails -- a fatal condition, not
// worth retrying, since a single ~10 s mono 16-bit buffer failing to allocate points at PSRAM
// exhaustion/corruption rather than transient pressure. Otherwise returns ESP_OK; a capture with
// no real speech (just noise/clicks), or one abandoned partway through by a button press, is a
// normal outcome, not an error, and comes back as a zeroed *out (audio_buf_t's own "no audio"
// contract).
esp_err_t audio_capture_utterance(audio_buf_t *out);

// SPEAK: play one mono PCM buffer through the amp. Resamples internally to the amp's fixed output
// rate if `audio` isn't already at that rate -- the normal TTS path is already at the amp's rate
// and never hits this. `move_mouth` drives the lip-sync motor (motors.h) off the audio envelope
// when true; pass false for playback that shouldn't move the mechanism (e.g. repeating a captured
// utterance back for debugging). Returns FISH_ERR_AUDIO_HW if the amp TX write fails,
// FISH_ERR_MOTOR_FAULT if nFAULT interrupts mouth animation, ESP_ERR_NO_MEM if a needed resample
// allocation fails; ESP_OK otherwise (including for empty/null audio, which is a no-op).
esp_err_t audio_play(const audio_buf_t *audio, bool move_mouth);
