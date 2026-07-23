// Transport layer: the fish<->backend contract. Mirrors the three calls the CLI reference
// client makes (client/billy_cli.py) — STT direct to whisper, the brain hop to the shim,
// TTS direct to Kokoro. This is the orchestrator seam (§8.1): only this layer changes if
// the fish ever moves from direct-HTTP to ESPHome/Home Assistant.
#pragma once
#include "hal.h"      // audio_buf_t
#include "esp_err.h"

// Per-sentence callback for the streamed reply, so TTS/playback pipelines with generation.
typedef void (*sentence_cb_t)(const char *sentence, void *ctx);

// Bring up networking: join WiFi (station) and block until an IP is acquired. Returns an error
// (without spinning) if credentials are missing or the join fails.
esp_err_t net_init(void);

// Block until the backend answers a health check, retrying with capped exponential backoff.
void net_wait_for_backend(void);

// STT: POST audio to whisper /inference -> transcript text.
esp_err_t net_stt(const audio_buf_t *audio, char *out_text, size_t out_len);

// Brain: POST {session, text} to the shim /v1/respond; invokes on_sentence for each spoken
// sentence as it streams back (SSE).
esp_err_t net_respond(const char *text, sentence_cb_t on_sentence, void *ctx);

// TTS: POST one sentence to Kokoro /v1/audio/speech -> audio.
esp_err_t net_tts(const char *sentence, audio_buf_t *out_audio);
