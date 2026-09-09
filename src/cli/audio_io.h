// Mic capture + speaker playback: PortAudio for I/O, soxr for output resampling. Host-side
// stand-in for the fish's I2S mic/amp (src/firmware/main/audio.c).
#pragma once
#include "cancel.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIO_SAMPLE_RATE 16000   // whisper wants 16 kHz mono

// Starts PortAudio and opens a persistent output stream at the default output device's native
// rate. Call once at startup, before audio_record_utterance/audio_play.
bool audio_init(void);
void audio_shutdown(void);

typedef enum
{
    AUDIO_RECORD_OK,
    AUDIO_RECORD_EMPTY,
    AUDIO_RECORD_INTERRUPTED,
    AUDIO_RECORD_ERROR,
} audio_record_result_t;

// Press Enter to start, speak, press Enter to stop. On AUDIO_RECORD_OK, writes a malloc'd mono
// buffer at AUDIO_SAMPLE_RATE to *out_audio (caller frees) and its length to *out_count.
audio_record_result_t audio_record_utterance(int16_t **out_audio, size_t *out_count,
                                             cancel_check_t cancel, const volatile void *cancel_ctx);

// Play mono PCM16 `audio`/`count`/`sample_rate` on the persistent output stream, resampling to
// the device's native rate first if they differ. Resampling in software (rather than requesting
// a mismatched rate from PortAudio and letting the OS mixer convert it) is deliberate -- the
// latter produced audible static in testing.
void audio_play(const int16_t *audio, size_t count, int sample_rate,
                cancel_check_t cancel, const volatile void *cancel_ctx);
