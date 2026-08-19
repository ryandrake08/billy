// Billy fish firmware — entry point and top-level turn loop.
#include "audio.h"
#include "motors.h"
#include "activation.h"
#include "sensors.h"
#include "status_led.h"
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

static void signal_motor_fault(void)
{
    led_set_status(LED_STATUS_ERROR);
    audio_error_tone();
}

// SPEAK worker: synthesize one streamed sentence and play it with mouth sync. Invoked by
// net_respond for each sentence, so playback pipelines with generation. Propagates (net_respond
// stops the stream and passes it up) for conditions the runloop needs to reboot over -- playback
// hardware failure or PSRAM exhaustion, which require a reboot, and a motor fault, which aborts
// the rest of the reply so the run loop can present the local safety error. Any ordinary TTS
// failure is handled locally (tone + keep going) since it's a per-sentence network hiccup.
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
        audio_error_tone();
        err = ESP_OK;
    }
    else if (err == ESP_OK && audio.count > 0)
    {
        err = audio_play(&audio, /* move_mouth = */ true);
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

    // We need to skip the idle wait if the user pushed the button and the device booted out of
    // deep sleep mode, and the photocell doesn't reject it as a dark-room false positive. Default
    // is to not skip the idle wait.
    bool skip_idle = false;

    // Read the raw wake pin first, before deciding whether to skip idle below.
    wake_pin_t wake_pin = activation_deep_sleep_wake_pin();
    if (wake_pin == WAKE_PIN_BUTTON)
    {
        ESP_LOGI(TAG, "deep-sleep boot: button pushed");

        // Test the photocell brightness to filter out false positives. If the room is bright
        // enough, we have a true activation-from-boot and need to skip the idle wait.
        skip_idle = sensors_photocell_bright_enough();
    }

    if (wake_pin == WAKE_PIN_MODE_SW)
    {
        ESP_LOGI(TAG, "deep-sleep boot: mode switch flipped");
    }

    for (;;)
    {
        // A single best-effort, bounded attempt -- must never meaningfully delay sleep or a
        // WAKEWORD-mode loop. If it fails because WiFi is not available or the server doesn't
        // respond, no big deal. We'll retry next turn.
        net_fetch_config(/* wait_for_backend = */ false);

        if (!skip_idle)
        {
            // Idle state -- fish is waiting to be activated.
            // In button mode, this is a deep sleep. Activating the button or switch will boot the device
            // In wakeword mode, activating the button or wakeword will continue to the next state
            // In wakeword mode, activating the switch will interrupt the wait and return back to idle
            led_set_status(LED_STATUS_IDLE);
            activation_prepare_sleep();
            activation_event_t event = activation_wait();
            if (event == ACTIVATION_MODE_SW)
            {
                // The mode switch flipped -- loop back so the next pass prepares and waits using
                // the newly selected mode.
                continue;
            }

            // A dark room rules the candidate out as a likely false positive -- a flaky button
            // contact, or background noise misfiring the wake-word detector -- rather than a
            // deliberate activation.
            if (!sensors_photocell_bright_enough())
            {
                continue;
            }
        }
        skip_idle = false;

        // A fault remains latched for the rest of its turn. Only a fresh user activation may
        // attempt recovery, with every PWM command cleared and nFAULT checked around driver wake.
        if (motors_faulted())
        {
            if (!motors_recover())
            {
                signal_motor_fault();
                continue;
            }
            ESP_LOGI(TAG, "motor fault recovered at activation; drivers parked until commanded");
        }

        // Prepare to listen -- fish plays a prompt tone and flaps its tail
        led_set_status(LED_STATUS_LISTEN);
        audio_prompt_tone();
        if (!motors_tail_flap())
        {
            signal_motor_fault();
            continue;
        }

        // Capture an utterance from the microphone
        audio_buf_t utterance = {0};
        if (audio_capture_utterance(&utterance) != ESP_OK)   // blocks until speech, then silence
        {
            // PSRAM allocation failure -- not worth retrying capture again against the same
            // exhausted heap. A fresh boot clears that heap state entirely, so recover by
            // rebooting rather than halting; flash the error status first so it's visible
            // even though the reboot (and BUTTON mode's own deep-sleep reboots) will clear it.
            led_set_status(LED_STATUS_ERROR);
            ESP_LOGE(TAG, "capture failed (PSRAM allocation) — rebooting");
            esp_restart();
        }

        // Debug: play the utterance straight back over the amp (no mouth motor) before it goes
        // to STT, so mic/acoustic quality can be checked by ear with no network round trip.
        // audio_play() resamples it from the mic's rate to the amp's rate itself.
        if (fish_config_get()->repeat_mode)
        {
            audio_play(&utterance, /* move_mouth = */ false);
        }

        // Transcribe the utterance using speech-to-text backend
        led_set_status(LED_STATUS_THINK);
        char transcript[256];
        esp_err_t stt_err = net_stt(&utterance, transcript, sizeof transcript);
        heap_caps_free(utterance.samples);   // PCM no longer needed after STT

        if (stt_err == ESP_ERR_NO_MEM)
        {
            // Same device-wide, will-just-recur condition as the capture failure above -- reboot
            // rather than tone-and-retry into the same exhausted heap.
            led_set_status(LED_STATUS_ERROR);
            ESP_LOGE(TAG, "STT failed (heap/PSRAM exhausted) — rebooting");
            esp_restart();
        }
        else if (stt_err != ESP_OK)
        {
            // Backend/network failure, not "nothing to say" -- distinct cue so the user doesn't
            // think the fish just didn't hear them and repeat themselves into the same failure.
            ESP_LOGW(TAG, "STT failed — back to idle");
            audio_error_tone();
            continue;
        }

        if (transcript[0] == '\0')
        {
            // Covers both real silence and a capture abandoned partway through by a button press
            // (audio_capture_utterance() returns a zeroed buffer for either) -- both are normal,
            // not errors, and both just need a plain trip back to idle.
            ESP_LOGI(TAG, "heard nothing — back to idle");
            continue;
        }

        // Pass utterance transcript to LLM backend shim app
        led_set_status(LED_STATUS_SPEAK);
        if (!motors_head_out())
        {
            signal_motor_fault();
            continue;
        }

        // The shim streams sentences; speak_sentence TTS+plays each as it arrives.
        esp_err_t respond_err = net_respond(transcript, speak_sentence, NULL);
        if (respond_err == FISH_ERR_AUDIO_HW || respond_err == ESP_ERR_NO_MEM)
        {
            // Both are device-wide conditions that will just recur on the next sentence -- the
            // speaker is broken, or PSRAM is exhausted -- so playing the usual error tone (which
            // itself may need PSRAM/the speaker) and continuing is pointless. A fresh boot clears
            // heap state and re-inits the amp/I2S from scratch, same rationale as the
            // capture-failure reboot above.
            led_set_status(LED_STATUS_ERROR);
            ESP_LOGE(TAG, "unrecoverable TTS/playback failure (%s) — rebooting",
                     esp_err_to_name(respond_err));
            esp_restart();
        }
        else if (respond_err == FISH_ERR_MOTOR_FAULT)
        {
            signal_motor_fault();
            continue;
        }
        else if (respond_err != ESP_OK)
        {
            ESP_LOGW(TAG, "brain hop failed");
            audio_error_tone();
        }

        // Relax now, once the whole reply is done -- not on any silence gap between streamed
        // sentences, a known failure mode in similar builds where the head never settles.
        if (!motors_head_relax())
        {
            signal_motor_fault();
            continue;
        }

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
    // LED first -- so a panic anywhere below (the various *_init()'s ESP_ERROR_CHECKs, etc.)
    // still leaves the status LED showing boot-white instead of unlit, giving some visible sign
    // the chip powered on at all.
    led_init();

    // Initialize Vmotor sensing before the motor fault task, so even an nFAULT line already low
    // at boot can be sampled safely in deferred task context.
    sensors_init();

    // Initialize the remaining hardware -- each owns an independent set of pins/peripherals.
    motors_init();
    audio_init();
    activation_init();

    // initialize wakeword subsystem
    if (!wakeword_init())
    {
        // Non-fatal: button mode is a complete fallback with no wake-word dependency.
        ESP_LOGW(TAG, "wake-word model failed to load — WAKEWORD mode won't detect; BUTTON mode still works");
    }

    // initialize network
    if (net_init() != ESP_OK)   // only fails on a config error (missing creds) -- not fixable by retrying
    {
        led_set_status(LED_STATUS_ERROR);
        ESP_LOGE(TAG, "WiFi config invalid — cannot bring up networking. Halting.");
        return;
    }

    // spawn the turn loop on its own task (see runloop_task's doc comment for the stack sizing)
    xTaskCreate(runloop_task, "runloop", 12288, NULL, 5, NULL);
}
