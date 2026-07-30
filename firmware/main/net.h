// Transport layer: the fish<->backend contract. Mirrors the three calls the CLI reference
// client makes (client/billy_cli.py) — STT direct to whisper, the brain hop to the shim,
// TTS direct to Kokoro. This is the orchestrator seam: only this layer changes if
// the fish ever moves from direct-HTTP to ESPHome/Home Assistant.
#pragma once
#include "hal.h"      // audio_buf_t
#include "esp_err.h"

// Per-sentence callback for the streamed reply, so TTS/playback pipelines with generation. voice
// is the Kokoro voice the shim chose for this sentence (it may vary sentence-to-sentence within
// one reply). A non-ESP_OK return (e.g. FISH_ERR_AUDIO_HW -- see hal.h) is treated as fatal:
// net_respond stops reading the stream and passes that same error back up as its own return value.
typedef esp_err_t (*sentence_cb_t)(const char *sentence, const char *voice, void *ctx);

// Bring up networking: start the WiFi driver and kick off the first join attempt, then return
// without waiting for an IP -- the connection is carried the rest of the way asynchronously, and
// is supervised (retried with backoff, indefinitely) for the life of the app. Only fails
// (without spinning) if credentials are missing.
esp_err_t net_init(void);

// STT: POST audio to whisper /inference -> transcript text.
esp_err_t net_stt(const audio_buf_t *audio, char *out_text, size_t out_len);

// Brain: POST {session, text} to the shim /v1/respond; invokes on_sentence for each spoken
// sentence (and the voice it should be spoken in) as it streams back (SSE).
esp_err_t net_respond(const char *text, sentence_cb_t on_sentence, void *ctx);

// TTS: POST one sentence to Kokoro /v1/audio/speech -> audio, in the given voice.
esp_err_t net_tts(const char *sentence, const char *voice, audio_buf_t *out_audio);
