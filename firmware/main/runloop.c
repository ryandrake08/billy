#include "runloop.h"
#include "hal.h"
#include "net.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stdbool.h>

static const char *TAG = "runloop";

static const char *state_name(fish_state_t s)
{
    switch (s)
    {
        case FISH_IDLE:     return "IDLE";
        case FISH_ACTIVATE: return "ACTIVATE";
        case FISH_LISTEN:   return "LISTEN";
        case FISH_THINK:    return "THINK";
        case FISH_SPEAK:    return "SPEAK";
    }
    return "?";
}

// SPEAK worker: synthesize one streamed sentence and play it with mouth sync. Invoked by
// net_respond for each sentence, so playback pipelines with generation.
//
// The head raises here, on the first sentence that actually has audio -- not earlier, at the
// THINK->SPEAK transition -- so it lines up with when sound actually starts instead of with the
// LLM/TTS latency before the first sentence is ready (that gap was visible as the head moving
// well before any audio played).
typedef struct
{
    bool head_raised;
} speak_ctx_t;

static void speak_sentence(const char *sentence, void *ctx)
{
    speak_ctx_t *sc = (speak_ctx_t *) ctx;
    audio_buf_t audio = {0};
    if (net_tts(sentence, &audio) == ESP_OK && audio.count > 0)
    {
        if (!sc->head_raised)
        {
            fish_hal_head_out();              // "I'm talking" -- now timed to the first real audio
            sc->head_raised = true;
        }
        fish_hal_play_with_mouth(&audio);
    }
    audio_buf_free(&audio);
}

// The task body. Runs on its own task (see runloop_start) — the per-turn work nests net_respond's
// SSE reader over net_tts and playback, plus esp_http_client under each, so it needs far more
// stack than the main task's ~3.5 KB default.
static void runloop_task(void *arg)
{
    (void) arg;
    fish_state_t state = FISH_IDLE;
    audio_buf_t utterance = {0};
    char transcript[256];

    for (;;)
    {
        ESP_LOGI(TAG, "state=%s", state_name(state));
        switch (state)
        {
            case FISH_IDLE:
                fish_hal_set_status(FISH_STATUS_IDLE);
                fish_hal_prepare_sleep();
                fish_hal_wait_for_wake();
                vTaskDelay(pdMS_TO_TICKS(200));       // a short rest beat between turns
                state = FISH_ACTIVATE;
                break;

            case FISH_ACTIVATE:
                fish_hal_set_status(FISH_STATUS_LISTEN);   // cue covers ACTIVATE through LISTEN
                fish_hal_prompt_tone();               // "ready — start talking"
                fish_hal_tail_flap();                 // "I'm listening"
                state = FISH_LISTEN;
                break;

            case FISH_LISTEN:
                fish_hal_capture_utterance(&utterance);   // blocks until speech, then silence
                state = FISH_THINK;
                break;

            case FISH_THINK:
                fish_hal_set_status(FISH_STATUS_THINK);
                if (net_stt(&utterance, transcript, sizeof transcript) != ESP_OK
                    || transcript[0] == '\0')
                {
                    audio_buf_free(&utterance);
                    ESP_LOGI(TAG, "heard nothing — listening again");
                    state = FISH_IDLE;
                }
                else
                {
                    audio_buf_free(&utterance);       // PCM no longer needed after STT
                    ESP_LOGI(TAG, "heard: \"%s\"", transcript);
                    state = FISH_SPEAK;
                }
                break;

            case FISH_SPEAK:
            {
                fish_hal_set_status(FISH_STATUS_SPEAK);
                // The shim streams sentences; speak_sentence TTS+plays each as it arrives, and
                // raises the head on the first one that actually has audio.
                speak_ctx_t sc = { .head_raised = false };
                net_respond(transcript, speak_sentence, &sc);
                fish_hal_head_relax();                // relax on response-complete, not silence
                state = FISH_IDLE;
                break;
            }
        }
    }
}

void runloop_start(void)
{
    xTaskCreate(runloop_task, "runloop", 12288, NULL, 5, NULL);
}
