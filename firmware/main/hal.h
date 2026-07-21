// Hardware-abstraction layer (SCOPING.md §8.1, "hardware"). Everything the app runloop needs
// from the physical fish, behind a stable interface. Implementations are stubs today (no board
// yet, Stage 3); they log intent so the runloop can be exercised on-target now.
#pragma once
#include <stddef.h>

// A block of captured or synthesized audio. The scaffold only tracks length; the real
// implementation will carry PCM samples (and stream, rather than buffer a whole utterance).
typedef struct
{
    size_t len;
} audio_buf_t;

void hal_init(void);

// IDLE: park the motors, mute the amp, and arm wake sources, then sleep.
void hal_prepare_sleep(void);
// Block until an activation event (button press, or wake word in always-on mode).
void hal_wait_for_wake(void);

// Body choreography (§6): a tail flap signals "I'm listening"; the head lifts to speak and
// relaxes when the response completes.
void hal_tail_flap(void);
void hal_head_out(void);
void hal_head_relax(void);

// LISTEN: capture mic audio until on-device VAD reports end-of-speech.
void hal_capture_utterance(audio_buf_t *out);

// SPEAK: play one synthesized chunk while driving the mouth motor from its RMS envelope.
void hal_play_with_mouth(const audio_buf_t *audio);
