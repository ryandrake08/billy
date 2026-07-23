// Hardware-abstraction layer (SCOPING.md §8.1, "hardware"). Everything the app runloop needs
// from the physical fish, behind a stable interface. Audio I/O (I²S mic + amp) is real as of
// Stage 3.1; the motor/wake functions are still stubs that log intent until those drivers land.
#pragma once
#include <stddef.h>

// A block of captured or synthesized audio. The scaffold only tracks length; the real
// implementation will carry PCM samples (and stream, rather than buffer a whole utterance).
typedef struct
{
    size_t len;
} audio_buf_t;

// Bring up the hardware: amp SD_MODE high + both I²S controllers (mic RX, amp TX).
void fish_hal_init(void);

// Stage 3.1 bench bring-up: play a test tone on the amp, then continuously log mic level so the
// mic and amp wiring can be verified before the full audio path exists. Does not return.
void fish_hal_selftest(void);

// IDLE: park the motors, mute the amp, and arm wake sources, then sleep.
void fish_hal_prepare_sleep(void);
// Block until an activation event (button press, or wake word in always-on mode).
void fish_hal_wait_for_wake(void);

// Body choreography (§6): a tail flap signals "I'm listening"; the head lifts to speak and
// relaxes when the response completes.
void fish_hal_tail_flap(void);
void fish_hal_head_out(void);
void fish_hal_head_relax(void);

// LISTEN: capture mic audio until on-device VAD reports end-of-speech.
void fish_hal_capture_utterance(audio_buf_t *out);

// SPEAK: play one synthesized chunk while driving the mouth motor from its RMS envelope.
void fish_hal_play_with_mouth(const audio_buf_t *audio);
