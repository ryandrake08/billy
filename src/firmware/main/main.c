// Billy fish firmware — entry point and top-level turn loop.
#include "hal.h"
#include "net.h"
#include "wakeword.h"
#include "fish_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include <stdbool.h>

static const char *TAG = "billy";

// SPEAK worker: synthesize one streamed sentence and play it with mouth sync. Invoked by
// net_respond for each sentence, so playback pipelines with generation. Propagates (net_respond
// stops the stream and passes it up) for conditions the runloop needs to reboot over -- playback
// hardware failure, or PSRAM exhaustion, which will just recur on the next sentence's allocation
// the same way fish_hal_capture_utterance's PSRAM failure recurs on retry. Any other TTS failure
// is handled locally (tone + keep going) since it's a per-sentence network hiccup, not a reason to
// abort the whole reply.
static esp_err_t speak_sentence(const char *sentence, const char *voice, void *ctx)
{
    (void) ctx;
    audio_buf_t audio = {0};
    esp_err_t err = net_tts(sentence, voice, &audio);
    if (err != ESP_OK && err != ESP_ERR_NO_MEM)
    {
        // Backend/network failure, not "nothing to say" -- distinct cue so the user doesn't
        // think the fish just finished speaking normally. Recoverable, so swallow it here rather
        // than propagating: net_respond keeps streaming the rest of the reply.
        ESP_LOGW(TAG, "TTS failed for sentence: \"%s\"", sentence);
        fish_hal_error_tone();
        err = ESP_OK;
    }
    else if (err == ESP_OK && audio.count > 0)
    {
        err = fish_hal_play(&audio, /* move_mouth = */ true);
    }
    heap_caps_free(audio.samples);
    return err;
}

// The turn loop task body: idle -> listen -> transcribe -> speak -> idle, driving the HAL and
// transport layers. Runs on its own task (see app_main) — the per-turn work nests net_respond's
// SSE reader over net_tts and playback, plus esp_http_client under each, so it needs far more
// stack than the main task's ~3.5 KB default.
static void runloop_task(void *arg)
{
    (void) arg;

    // fish_hal_boot_cause() validates a button-pin wake against the photocell (fish_config_get()),
    // which by default is still whatever was compiled in -- nothing has fetched config yet at
    // this point in boot. So a config fetch needs to be forced *before* that validation runs, for
    // it to have a real shot at a fresh (or shim-overridden) threshold. That decision can't be
    // based on fish_hal_boot_cause()'s own result -- that's the very thing the fetch would
    // affect -- so it's based on the raw wake pin instead (fish_hal_deep_sleep_wake_pin()), which
    // has no such dependency.
    if (fish_hal_deep_sleep_wake_pin() == FISH_WAKE_PIN_BUTTON)
    {
        net_fetch_config(/* wait_for_backend = */ true);
    }

    // A button press in BUTTON mode wakes the chip from deep sleep via a full reboot -- landing
    // in idle here would immediately re-sleep on that same press (see fish_boot_cause_t's doc
    // comment) without ever using it, so the first turn skips idle and treats the press that
    // caused it as the activation event. A mode-switch reboot (or no such cause at all) goes
    // through idle normally, like a cold boot.
    bool skip_idle = (fish_hal_boot_cause() == FISH_BOOT_BUTTON);

    for (;;)
    {
        // Best-effort, bounded -- picks up any shim-side config change on every cycle, so a
        // WAKEWORD-mode fish (which never reboots) can be retuned live with no reboot at all.
        // The one case that needs a forced, longer wait (a button-caused wake, about to skip
        // idle below and go straight into a turn that needs the network right after) already got
        // it above, before fish_hal_boot_cause() ran -- this call always stays light so it never
        // meaningfully delays sleep or a WAKEWORD-mode loop.
        net_fetch_config(/* wait_for_backend = */ false);

        if (!skip_idle)
        {
            // Idle state -- fish is waiting to be activated.
            // In button mode, this is a deep sleep. Activating the button or switch will boot the device
            // In wakeword mode, activating the button or wakeword will continue to the next state
            // In wakeword mode, activating the switch will interrupt the wait and return back to idle
            fish_hal_set_status(FISH_STATUS_IDLE);
            fish_hal_prepare_sleep();
            if (!fish_hal_wait_for_wake())
            {
                // The mode switch flipped -- loop back so the next pass calls
                // fish_hal_prepare_sleep() again with the fresh mode (real deep sleep if it's now
                // BUTTON mode) instead of continuing to poll in the old mode's style.
                continue;
            }
        }
        skip_idle = false;

        // Prepare to listen -- fish plays a prompt tone and flaps its tail
        fish_hal_set_status(FISH_STATUS_LISTEN);
        fish_hal_prompt_tone();
        fish_hal_tail_flap();

        // Capture an utterance from the microphone
        audio_buf_t utterance = {0};
        if (fish_hal_capture_utterance(&utterance) != ESP_OK)   // blocks until speech, then silence
        {
            // PSRAM allocation failure -- not worth retrying capture again against the same
            // exhausted heap. A fresh boot clears that heap state entirely, so recover by
            // rebooting rather than halting; flash the error status first so it's visible
            // even though the reboot (and BUTTON mode's own deep-sleep reboots) will clear it.
            fish_hal_set_status(FISH_STATUS_ERROR);
            ESP_LOGE(TAG, "capture failed (PSRAM allocation) — rebooting");
            esp_restart();
        }

        // Debug: play the utterance straight back over the amp (no mouth motor) before it goes
        // to STT, so mic/acoustic quality can be checked by ear with no network round trip.
        // fish_hal_play() resamples it from MIC_SAMPLE_RATE to AMP_SAMPLE_RATE itself.
        if (fish_config_get()->repeat_mode)
        {
            fish_hal_play(&utterance, /* move_mouth = */ false);
        }

        // Transcribe the utterance using speech-to-text backend
        fish_hal_set_status(FISH_STATUS_THINK);
        char transcript[256];
        esp_err_t stt_err = net_stt(&utterance, transcript, sizeof transcript);
        heap_caps_free(utterance.samples);   // PCM no longer needed after STT

        if (stt_err == ESP_ERR_NO_MEM)
        {
            // Same device-wide, will-just-recur condition as the capture failure above -- reboot
            // rather than tone-and-retry into the same exhausted heap.
            fish_hal_set_status(FISH_STATUS_ERROR);
            ESP_LOGE(TAG, "STT failed (heap/PSRAM exhausted) — rebooting");
            esp_restart();
        }
        else if (stt_err != ESP_OK)
        {
            // Backend/network failure, not "nothing to say" -- distinct cue so the user doesn't
            // think the fish just didn't hear them and repeat themselves into the same failure.
            ESP_LOGW(TAG, "STT failed — back to idle");
            fish_hal_error_tone();
            continue;
        }

        if (transcript[0] == '\0')
        {
            ESP_LOGI(TAG, "heard nothing — back to idle");
            continue;
        }

        // Pass utterance transcript to LLM backend shim app
        fish_hal_set_status(FISH_STATUS_SPEAK);
        fish_hal_head_out();

        // The shim streams sentences; speak_sentence TTS+plays each as it arrives.
        esp_err_t respond_err = net_respond(transcript, speak_sentence, NULL);
        if (respond_err == FISH_ERR_AUDIO_HW || respond_err == ESP_ERR_NO_MEM)
        {
            // Both are device-wide conditions that will just recur on the next sentence -- the
            // speaker is broken, or PSRAM is exhausted -- so playing the usual error tone (which
            // itself may need PSRAM/the speaker) and continuing is pointless. A fresh boot clears
            // heap state and re-inits the amp/I2S from scratch, same rationale as the
            // capture-failure reboot above.
            fish_hal_set_status(FISH_STATUS_ERROR);
            ESP_LOGE(TAG, "unrecoverable TTS/playback failure (%s) — rebooting",
                     esp_err_to_name(respond_err));
            esp_restart();
        }
        else if (respond_err != ESP_OK)
        {
            ESP_LOGW(TAG, "brain hop failed");
            fish_hal_error_tone();
        }

        // Return head to relaxed state
        fish_hal_head_relax();

        // Cumulative low-water mark since boot -- the worst-case PSRAM usage any turn has hit
        // so far, not just this one. Bench this against a 2 MB budget (N4R2 candidate) before
        // committing to it: run several turns, including a long utterance near capture_max_ms
        // and long TTS sentences, then check this log's peak-used figure stays under ~2 MB.
        ESP_LOGI(TAG, "PSRAM watermark: %u bytes peak used of %u total",
                 (unsigned) (heap_caps_get_total_size(MALLOC_CAP_SPIRAM)
                             - heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)),
                 (unsigned) heap_caps_get_total_size(MALLOC_CAP_SPIRAM));

        // loop back to idle
    }
}

void app_main(void)
{
    // initialize all hardware
    fish_hal_init();

    // initialize wakeword subsystem
    if (!wakeword_init())
    {
        // Non-fatal: button mode is a complete fallback with no wake-word dependency.
        ESP_LOGW(TAG, "wake-word model failed to load — WAKEWORD mode won't detect; BUTTON mode still works");
    }

    // initialize network
    if (net_init() != ESP_OK)   // only fails on a config error (missing creds) -- not fixable by retrying
    {
        fish_hal_set_status(FISH_STATUS_ERROR);
        ESP_LOGE(TAG, "WiFi config invalid — cannot bring up networking. Halting.");
        return;
    }

    // spawn the turn loop on its own task (see runloop_task's doc comment for the stack sizing)
    xTaskCreate(runloop_task, "runloop", 12288, NULL, 5, NULL);
}
