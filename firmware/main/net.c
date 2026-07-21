#include "net.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "net";

// Stub transport. The real implementation will join WiFi and use esp_http_client against the
// homelab endpoints (shim :8000, whisper :8081, Kokoro :8880). For now these log and return
// canned values so the runloop exercises the full turn shape without a network.

esp_err_t net_init(void)
{
    ESP_LOGI(TAG, "init (stub) — WiFi + HTTP client not wired yet");
    return ESP_OK;
}

esp_err_t net_stt(const audio_buf_t *audio, char *out_text, size_t out_len)
{
    (void) audio;
    ESP_LOGI(TAG, "STT -> whisper /inference (stub)");
    strlcpy(out_text, "what time is it", out_len);
    return ESP_OK;
}

esp_err_t net_respond(const char *text, sentence_cb_t on_sentence, void *ctx)
{
    ESP_LOGI(TAG, "brain -> shim /v1/respond, text=\"%s\" (stub)", text);
    // Two canned sentences to exercise pipelined per-sentence playback.
    on_sentence("Well now, I'm just a fish on a wall.", ctx);
    on_sentence("I can't see a clock from up here, partner.", ctx);
    return ESP_OK;
}

esp_err_t net_tts(const char *sentence, audio_buf_t *out_audio)
{
    ESP_LOGI(TAG, "TTS -> Kokoro /v1/audio/speech: \"%s\" (stub)", sentence);
    out_audio->len = strlen(sentence);
    return ESP_OK;
}
