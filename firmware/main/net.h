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

// Fetch runtime-tunable constants from the shim's GET /v1/config and apply them (fish_config.h).
// Called once at the top of every runloop cycle (main.c) rather than once at boot, so a config
// change on the shim reaches a long-lived WAKEWORD-mode fish with no reboot needed.
//
// wait_for_backend=false: a single best-effort, bounded (a few seconds) attempt. On any failure
// (no WiFi, shim unreachable, bad response) the values already in effect -- compiled-in or from
// an earlier fetch -- are left untouched. Used for the routine idle-cycle call, which must never
// meaningfully delay sleep or a WAKEWORD-mode loop.
//
// wait_for_backend=true: retries for longer (still bounded, never forever) until WiFi is up and
// the fetch succeeds. Needed on a wake path that skips straight into a turn with no idle/wait
// gate first (a button-caused wake, main.c) -- since WiFi's join is fully async, that turn's STT
// call could otherwise easily run before the join completes.
esp_err_t net_fetch_config(bool wait_for_backend);

// STT: POST audio to whisper /inference -> transcript text.
esp_err_t net_stt(const audio_buf_t *audio, char *out_text, size_t out_len);

// Brain: POST {session, text} to the shim /v1/respond; invokes on_sentence for each spoken
// sentence (and the voice it should be spoken in) as it streams back (SSE).
esp_err_t net_respond(const char *text, sentence_cb_t on_sentence, void *ctx);

// TTS: POST one sentence to Kokoro /v1/audio/speech -> audio, in the given voice.
esp_err_t net_tts(const char *sentence, const char *voice, audio_buf_t *out_audio);
