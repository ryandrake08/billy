// Hardware-abstraction layer. Everything the app runloop needs from the physical
// fish, behind a stable interface. Audio I/O (I²S mic + amp) and activation (mode switch + button,
// bench-wired as bare jumpers) are real; the wake-word detector and the motor functions are still
// stubs that log intent until those land.
#pragma once
#include <stddef.h>
#include <stdint.h>

// A block of mono 16-bit PCM. `samples` is heap_caps-allocated in PSRAM; free with
// audio_buf_free(). A zeroed buffer (samples == NULL, count == 0) means "no audio".
typedef struct
{
    int16_t *samples;
    size_t   count;        // number of samples
    int      sample_rate;  // Hz
} audio_buf_t;

// Release a buffer's PSRAM samples and zero it. Safe on an already-empty buffer.
void audio_buf_free(audio_buf_t *buf);

// Bring up the hardware: amp SD_MODE high + both I²S controllers (mic RX enabled, amp TX ready).
void fish_hal_init(void);

// Bench audio self-test (not in the E2E boot path): 440 Hz tone, then a live mic-level log.
void fish_hal_selftest(void);

// A short "ready — start talking" beep on the amp.
void fish_hal_prompt_tone(void);

// IDLE: park the motors, mute the amp, and arm wake sources, then sleep.
void fish_hal_prepare_sleep(void);
// Block until an activation event (button press, or wake word in always-on mode).
void fish_hal_wait_for_wake(void);

// Body choreography (§6): a tail flap signals "I'm listening"; the head lifts to speak and
// relaxes when the response completes.
void fish_hal_tail_flap(void);
void fish_hal_head_out(void);
void fish_hal_head_relax(void);

// LISTEN: energy-VAD capture — wait for speech onset, capture until ~0.8 s of silence (or a
// hard cap). Allocates out->samples in PSRAM; the caller frees it with audio_buf_free().
void fish_hal_capture_utterance(audio_buf_t *out);

// SPEAK: play one mono PCM chunk at its own sample rate (mouth-motor sync comes later).
void fish_hal_play_with_mouth(const audio_buf_t *audio);
