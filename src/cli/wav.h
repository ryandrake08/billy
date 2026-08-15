// Hand-rolled canonical PCM16 WAV: just enough to build the mic upload and parse the TTS
// response, without pulling in libsndfile.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WAV_HEADER_BYTES 44

// Build a 44-byte canonical PCM WAV header for `nsamples` mono 16-bit samples at `rate` Hz.
void wav_write_header(uint8_t out[WAV_HEADER_BYTES], uint32_t nsamples, uint32_t rate);

typedef struct
{
    int16_t *samples;    // malloc'd mono PCM16; caller frees
    size_t   count;
    uint32_t sample_rate;
} wav_pcm16_t;

// Parse a RIFF/WAVE buffer into mono 16-bit PCM, downmixing stereo if present. Only PCM
// (format 1), 16-bit, mono or stereo source WAVs are supported -- anything else is an error
// rather than a silent misdecode.
bool wav_parse_pcm16(const uint8_t *buf, size_t len, wav_pcm16_t *out);
