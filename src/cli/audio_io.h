// Mic capture + speaker playback: PortAudio for I/O, soxr for output resampling. Host-side
// stand-in for the fish's I2S mic/amp (src/firmware/main/audio.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIO_SAMPLE_RATE 16000   // whisper wants 16 kHz mono

// Starts PortAudio and opens a persistent output stream at the default output device's native
// rate. Call once at startup, before audio_record_utterance/audio_play.
bool audio_init(void);
void audio_shutdown(void);

// Press Enter to start, speak, press Enter to stop. Returns a malloc'd int16 mono buffer at
// AUDIO_SAMPLE_RATE (caller frees) and sets *out_count to its length, or NULL if nothing was
// captured.
int16_t *audio_record_utterance(size_t *out_count);

// Play mono PCM16 `audio`/`count`/`sample_rate` on the persistent output stream, resampling to
// the device's native rate first if they differ. Resampling in software (rather than requesting
// a mismatched rate from PortAudio and letting the OS mixer convert it) is deliberate -- the
// latter produced audible static in testing.
void audio_play(const int16_t *audio, size_t count, int sample_rate);
