// Runtime-tunable firmware constants, fetched from the shim's GET /v1/config
// (net_fetch_config(), net.c) at the top of every runloop cycle, so they can be retuned after
// final assembly with an edit-and-restart on the shim, not a firmware rebuild+reflash. Each
// field's initializer in fish_config.c is also this build's compiled-in fallback --
// fish_config_get() always returns something sane even if the shim is unreachable or a field is
// missing/unparseable in its response.
#pragma once
#include <stdbool.h>

typedef struct
{
    // VAD (voice-activity detection) -- audio.c audio_capture_utterance()
    int vad_onset_rms;
    int vad_silence_ms;
    int vad_min_voiced_ms;
    int vad_drain_ms;
    int capture_max_ms;

    // Photocell wake gate -- activation.c activation_wait_for_wake()
    int photocell_wake_threshold;

    // Mouth lip-sync envelope -- audio.c's playback envelope follower
    float mouth_env_ref;
    float mouth_env_attack;
    float mouth_env_release;
    float mouth_mid_threshold;
    float mouth_open_threshold;
    int   mouth_mid_duty_pct;

    // Tail choreography timing -- motors.c motors_tail_flap()
    int tail_flap_ms;
    int tail_settle_ms;

    // Backend HTTP timeouts -- net.c
    int stt_timeout_ms;
    int respond_timeout_ms;
    int tts_timeout_ms;

    // Debug -- main.c runloop_task(): play a just-captured utterance back over the amp (no mouth
    // motor) before sending it to STT, so mic/acoustic quality can be checked by ear with no
    // network round trip. audio.c audio_play() resamples it to AMP_SAMPLE_RATE automatically.
    bool repeat_mode;
} fish_config_t;

// Always returns a valid config -- the compiled-in defaults until/unless net_fetch_config()
// successfully overwrites some or all fields.
const fish_config_t *fish_config_get(void);

// Parse a flat {"key": number, ...} JSON object (the shim's GET /v1/config body) and overwrite
// only the fields present in it. A field that's missing or fails to parse keeps whatever value
// it already had (a compiled-in default, or a previous fetch's) -- a partial or malformed
// response degrades gracefully instead of clobbering good values with zeros.
void fish_config_apply_json(const char *json);
