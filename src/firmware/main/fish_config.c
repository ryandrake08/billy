#include "fish_config.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Compiled-in defaults -- also this build's fallback for any field the shim's response doesn't
// include (the shim only ever sends deltas from these, src/shim/fish_config.py's OVERRIDES).
static fish_config_t s_config = {
    .vad_onset_rms            = 6000,       // 24-bit scale: idle floor ~2000, speech >7000
    .vad_silence_ms           = 600,        // end the turn after this much sub-threshold audio
    .vad_min_voiced_ms        = 250,        // reject a capture with less real speech than this
    .vad_drain_ms             = 250,        // discard the mic's buffered prompt tone first
    .capture_max_ms           = 10000,      // hard cap on one utterance

    .photocell_wake_threshold = 50,         // bench-calibrated

    .mouth_env_ref            = 6000.0f,    // RMS that saturates the envelope at 1.0
    .mouth_env_attack         = 0.6f,
    .mouth_env_release        = 0.15f,
    .mouth_mid_threshold      = 0.3f,       // envelope above this -> MID; below -> CLOSED
    .mouth_open_threshold     = 0.65f,      // envelope above this -> OPEN; below -> MID
    .mouth_mid_duty_pct       = 80,         // middle mouth position duty cycle

    .tail_flap_ms             = 250,
    .tail_settle_ms           = 350,

    .stt_timeout_ms           = 30000,
    .respond_timeout_ms       = 120000,
    .tts_timeout_ms           = 120000,

    .repeat_mode              = false,
};

const fish_config_t *fish_config_get(void)
{
    return &s_config;
}

// Find a top-level `"key": <number>` in a flat JSON object and parse the number. No nesting,
// strings, or arrays to handle -- same hand-rolled, no-cJSON approach as net.c's json_get_string.
static bool json_get_number(const char *json, const char *key, double *out)
{
    char pat[48];
    int patlen = snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += patlen;
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) return false;
    *out = v;
    return true;
}

// Find a top-level `"key": true|false` in a flat JSON object and parse the boolean. Same
// hand-rolled key lookup as json_get_number, for the one field that isn't numeric.
static bool json_get_bool(const char *json, const char *key, bool *out)
{
    char pat[48];
    int patlen = snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += patlen;
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (strncmp(p, "true", 4) == 0)  { *out = true;  return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

void fish_config_apply_json(const char *json)
{
    double v;
    bool b;
    if (json_get_number(json, "vad_onset_rms", &v))              s_config.vad_onset_rms = (int) v;
    if (json_get_number(json, "vad_silence_ms", &v))             s_config.vad_silence_ms = (int) v;
    if (json_get_number(json, "vad_min_voiced_ms", &v))          s_config.vad_min_voiced_ms = (int) v;
    if (json_get_number(json, "vad_drain_ms", &v))               s_config.vad_drain_ms = (int) v;
    if (json_get_number(json, "capture_max_ms", &v))             s_config.capture_max_ms = (int) v;

    if (json_get_number(json, "photocell_wake_threshold", &v))   s_config.photocell_wake_threshold = (int) v;

    if (json_get_number(json, "mouth_env_ref", &v))              s_config.mouth_env_ref = (float) v;
    if (json_get_number(json, "mouth_env_attack", &v))           s_config.mouth_env_attack = (float) v;
    if (json_get_number(json, "mouth_env_release", &v))          s_config.mouth_env_release = (float) v;
    if (json_get_number(json, "mouth_mid_threshold", &v))        s_config.mouth_mid_threshold = (float) v;
    if (json_get_number(json, "mouth_open_threshold", &v))       s_config.mouth_open_threshold = (float) v;
    if (json_get_number(json, "mouth_mid_duty_pct", &v))         s_config.mouth_mid_duty_pct = (int) v;

    if (json_get_number(json, "tail_flap_ms", &v))               s_config.tail_flap_ms = (int) v;
    if (json_get_number(json, "tail_settle_ms", &v))             s_config.tail_settle_ms = (int) v;

    if (json_get_number(json, "stt_timeout_ms", &v))             s_config.stt_timeout_ms = (int) v;
    if (json_get_number(json, "respond_timeout_ms", &v))         s_config.respond_timeout_ms = (int) v;
    if (json_get_number(json, "tts_timeout_ms", &v))             s_config.tts_timeout_ms = (int) v;

    if (json_get_bool(json, "repeat_mode", &b))                  s_config.repeat_mode = b;
}
