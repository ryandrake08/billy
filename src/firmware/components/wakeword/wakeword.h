// Wake-word detector — microWakeWord streaming model on TFLite-Micro, behind a C interface so C
// firmware can drive it without touching C++/TFLM. Ported from the ESPHome micro_wake_word
// component (github.com/esphome/esphome, Apache-2.0) via the bare-ESP-IDF reference at
// github.com/0xD34D/micro_wake_word_standalone; see wakeword.cpp and models/ATTRIBUTION.md for
// the exact provenance of the ported logic and the model.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// wakeword_feed() must be called with a multiple of this many samples (10 ms @ 16 kHz — the
// model's feature-slice step). MIC_SAMPLE_RATE in audio.c is assumed to be 16 kHz throughout.
#define WAKEWORD_STEP_SAMPLES 160

// Loads the audio frontend and the streaming model (trained "Hey Billy" model — see
// models/ATTRIBUTION.md). Call once at boot, after PSRAM is up. Allocates the
// model's tensor arenas in PSRAM and keeps them resident (a few tens of KB; negligible on the
// N8R8's 8 MB). Returns false on failure (logs the reason).
bool wakeword_init(void);

// Clears the sliding-window probabilities and warm-up counter. Call once before each fresh
// listening session so a stale window from a prior session can't bias the next detection.
void wakeword_reset(void);

// Feeds n_samples of 16 kHz mono PCM (must be a multiple of WAKEWORD_STEP_SAMPLES — a caller
// wanting single-step granularity passes exactly WAKEWORD_STEP_SAMPLES per call). Returns true the
// moment the wake word is detected within this call; false otherwise. Safe to call even if
// wakeword_init() failed (always returns false in that case).
bool wakeword_feed(const int16_t *pcm, size_t n_samples);

#ifdef __cplusplus
}
#endif
