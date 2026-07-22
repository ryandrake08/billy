#include "hal.h"
#include "board.h"
#include "esp_log.h"

static const char *TAG = "hal";

// Every function here is a stub until the bench hardware exists (Stage 3). Each logs what the
// real driver will do so the runloop's control flow can be verified on-target now.

void fish_hal_init(void)
{
    ESP_LOGI(TAG, "init (stub) — I2S mic/amp, 2x DRV8833 motors, button/switch/photocell");
}

void fish_hal_prepare_sleep(void)
{
    ESP_LOGI(TAG, "prepare sleep — both DRV8833s nSLEEP low, amp muted, arm button wake on GPIO %d",
             BOARD_BUTTON);
}

void fish_hal_wait_for_wake(void)
{
    ESP_LOGI(TAG, "waiting for wake (stub returns immediately)");
}

void fish_hal_tail_flap(void)
{
    ESP_LOGI(TAG, "tail flap — 'I'm listening'");
}

void fish_hal_head_out(void)
{
    ESP_LOGI(TAG, "head out — 'I'm talking'");
}

void fish_hal_head_relax(void)
{
    ESP_LOGI(TAG, "head relax");
}

void fish_hal_capture_utterance(audio_buf_t *out)
{
    ESP_LOGI(TAG, "capture mic until VAD end-of-speech (stub)");
    out->len = 0;
}

void fish_hal_play_with_mouth(const audio_buf_t *audio)
{
    ESP_LOGI(TAG, "play %u samples + mouth PWM from RMS envelope (stub)", (unsigned) audio->len);
}
