#include "runloop.h"
#include "hal.h"
#include "net.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

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
static void speak_sentence(const char *sentence, void *ctx)
{
    (void) ctx;
    audio_buf_t audio;
    if (net_tts(sentence, &audio) == ESP_OK)
    {
        hal_play_with_mouth(&audio);
    }
}

void runloop_run(void)
{
    fish_state_t state = FISH_IDLE;
    audio_buf_t utterance = {0};
    char transcript[256];

    for (;;)
    {
        ESP_LOGI(TAG, "state=%s", state_name(state));
        switch (state)
        {
            case FISH_IDLE:
                hal_prepare_sleep();
                hal_wait_for_wake();
                state = FISH_ACTIVATE;
                break;

            case FISH_ACTIVATE:
                hal_tail_flap();                 // "I'm listening" (§6)
                state = FISH_LISTEN;
                break;

            case FISH_LISTEN:
                hal_capture_utterance(&utterance);
                state = FISH_THINK;
                break;

            case FISH_THINK:
                if (net_stt(&utterance, transcript, sizeof transcript) != ESP_OK
                    || transcript[0] == '\0')
                {
                    ESP_LOGI(TAG, "heard nothing — back to sleep");
                    state = FISH_IDLE;
                }
                else
                {
                    ESP_LOGI(TAG, "heard: \"%s\"", transcript);
                    hal_head_out();              // "I'm talking" (§6)
                    state = FISH_SPEAK;
                }
                break;

            case FISH_SPEAK:
                // The shim streams sentences; speak_sentence TTS+plays each as it arrives.
                net_respond(transcript, speak_sentence, NULL);
                hal_head_relax();                // relax on response-complete, not silence (§6)
                state = FISH_IDLE;
                break;
        }

        // Pace the scaffold so the stubbed loop reads clearly in the serial monitor. The real
        // runloop blocks in hal_wait_for_wake instead of spinning.
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
