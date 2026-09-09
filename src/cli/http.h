// Transport layer: the fish<->backend contract -- STT direct to whisper, the brain hop to the
// shim, TTS direct to Kokoro. Mirrors src/firmware/main/net.h's wire contract; this is the host
// side of the same protocol.
#pragma once
#include "cancel.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Must be called once before any other http_* call, from the main thread before any worker
// thread starts (libcurl's global init is not thread-safe).
void http_global_init(void);
void http_global_cleanup(void);

// STT: POST audio (packaged as a WAV) to whisper /inference -> transcript text.
bool http_stt(const char *stt_url, const int16_t *pcm, size_t nsamples, uint32_t sample_rate,
              char *out_text, size_t out_len, long timeout_ms,
              cancel_check_t cancel, const volatile void *cancel_ctx);

// Best-effort session reset; failure is logged, not fatal (stale history just lingers).
void http_reset(const char *shim_url, const char *session, long timeout_ms,
                cancel_check_t cancel, const volatile void *cancel_ctx);

// Per-sentence callback for the streamed reply. voice is NULL if the shim omitted it. Returning
// false stops reading the stream early and makes http_respond_stream itself return false.
typedef bool (*sentence_cb_t)(const char *sentence, const char *voice, void *ctx);

// Brain: POST {session, text} to the shim /v1/respond; invokes on_sentence for each spoken
// sentence as it streams back (SSE) before returning.
bool http_respond_stream(const char *shim_url, const char *session, const char *text,
                          sentence_cb_t on_sentence, void *ctx, long timeout_ms,
                          cancel_check_t cancel, const volatile void *cancel_ctx);

// TTS: POST one sentence to Kokoro /v1/audio/speech -> WAV bytes (caller frees with free()).
bool http_tts(const char *tts_url, const char *voice, const char *sentence,
              uint8_t **out_wav, size_t *out_len, long timeout_ms,
              cancel_check_t cancel, const volatile void *cancel_ctx);
