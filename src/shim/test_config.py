#!/usr/bin/env python3
"""Unit tests for the shim's runtime-config payload. No pytest dependency — run directly:

    ./.venv/bin/python test_config.py
"""
from fish_config import OVERRIDES

# Every field the firmware's fish_config_t actually knows about (fish_config.h). OVERRIDES may
# set any subset of these, or none, but never a field outside this set -- there's no DEFAULTS
# dict here to structurally catch a typo, so this is what catches it instead.
VALID_KEYS = {
    "vad_onset_rms", "vad_noise_margin", "vad_silence_ms", "vad_min_voiced_ms", "vad_drain_ms",
    "capture_max_ms",
    "mouth_env_ref", "mouth_env_attack", "mouth_env_release",
    "mouth_mid_threshold", "mouth_open_threshold", "mouth_mid_duty_pct",
    "tail_flap_ms", "tail_settle_ms",
    "stt_timeout_ms", "respond_timeout_ms", "tts_timeout_ms",
}


def check(got, want, label):
    assert got == want, f"{label}: expected {want!r}, got {got!r}"


def test_overrides_are_all_known_fields():
    unknown = set(OVERRIDES) - VALID_KEYS
    check(unknown, set(), "OVERRIDES keys not recognized by the firmware (typo?)")


def test_overrides_values_are_numbers():
    for key, value in OVERRIDES.items():
        assert isinstance(value, (int, float)), f"{key}: expected a number, got {value!r}"


if __name__ == "__main__":
    test_overrides_are_all_known_fields()
    test_overrides_values_are_numbers()
    print("ok — all shim config tests passed")
