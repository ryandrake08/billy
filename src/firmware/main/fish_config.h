// Runtime-tunable firmware constants, fetched from the shim's GET /v1/config
// (net_fetch_config(), net.c) at the top of every runloop cycle, so they can be retuned after
// final assembly with an edit-and-restart on the shim, not a firmware rebuild+reflash. Each
// field's initializer in fish_config.c is also this build's compiled-in fallback --
// fish_config_get() always returns something sane even if the shim is unreachable or a field is
// missing/unparseable in its response.
#pragma once

typedef struct
{
    // VAD (voice-activity detection) -- hal.c fish_hal_capture_utterance()
    int vad_onset_rms;
    int vad_silence_ms;
    int vad_min_voiced_ms;
    int vad_drain_ms;
    int capture_max_ms;

    // Photocell wake gate -- hal.c fish_hal_wait_for_wake()
    int photocell_wake_threshold;

    // Mouth lip-sync envelope -- hal.c's playback envelope follower
    float mouth_env_ref;
    float mouth_env_attack;
    float mouth_env_release;
    float mouth_mid_threshold;
    float mouth_open_threshold;
    int   mouth_mid_duty_pct;

    // Tail choreography timing -- hal.c fish_hal_tail_flap()
    int tail_flap_ms;
    int tail_settle_ms;

    // Backend HTTP timeouts -- net.c
    int stt_timeout_ms;
    int respond_timeout_ms;
    int tts_timeout_ms;
} fish_config_t;

// Always returns a valid config -- the compiled-in defaults until/unless net_fetch_config()
// successfully overwrites some or all fields.
const fish_config_t *fish_config_get(void);

// Parse a flat {"key": number, ...} JSON object (the shim's GET /v1/config body) and overwrite
// only the fields present in it. A field that's missing or fails to parse keeps whatever value
// it already had (a compiled-in default, or a previous fetch's) -- a partial or malformed
// response degrades gracefully instead of clobbering good values with zeros.
void fish_config_apply_json(const char *json);
