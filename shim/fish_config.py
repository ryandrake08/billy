"""Runtime-tunable firmware constants, served over GET /v1/config so they can be retuned after
final assembly without a firmware rebuild+reflash.

OVERRIDES holds only the values currently being trialed -- the firmware already carries its own
compiled-in default for every field (fish_config.c) and only overwrites a field when it's present
in the response, so there's nothing to gain by duplicating those defaults here. Empty in steady
state; add a key here to change it on the fish's next config fetch, remove it to fall back to the
firmware's own default.
"""

OVERRIDES = {
    # Every field below is commented out with the firmware's current compiled-in default
    # (fish_config.c) -- uncomment one to override it on the fish's next config fetch; re-comment
    # (or delete) to fall back to that default again. Keep the values shown here in sync with
    # fish_config.c by hand if that file's own defaults ever change.

    # VAD (voice-activity detection) -- hal.c fish_hal_capture_utterance()
    # "vad_onset_rms": 6000,        # 24-bit scale: idle floor ~2000, speech >7000
    # "vad_silence_ms": 600,        # end the turn after this much sub-threshold audio
    # "vad_min_voiced_ms": 250,     # reject a capture with less real speech than this (clicks)
    # "vad_drain_ms": 250,          # discard the mic's buffered prompt tone before listening
    # "capture_max_ms": 10000,      # hard cap on one utterance

    # Photocell wake gate -- hal.c fish_hal_wait_for_wake() / fish_hal_boot_cause()
    # "photocell_wake_threshold": 100,

    # Mouth lip-sync envelope -- hal.c's playback envelope follower
    # "mouth_env_ref": 6000.0,        # RMS that saturates the envelope at 1.0 (16-bit PCM scale)
    # "mouth_env_attack": 0.6,
    # "mouth_env_release": 0.15,
    # "mouth_mid_threshold": 0.3,     # envelope above this -> MID; below -> CLOSED
    # "mouth_open_threshold": 0.65,   # envelope above this -> OPEN (full duty); below -> MID
    # "mouth_mid_duty_pct": 80,       # WIRING.md §6.1: the distinct partial-deflection duty

    # Tail choreography timing -- hal.c fish_hal_tail_flap()
    # "tail_flap_ms": 250,     # one flap: drive out, then let the spring return it
    # "tail_settle_ms": 350,   # spring-return travel + mechanical ring-down before it's safe to listen

    # Backend HTTP timeouts -- net.c
    # "stt_timeout_ms": 30000,        # whisper.cpp transcribing the whole utterance
    # "respond_timeout_ms": 120000,   # shim SSE stream: LLM generation across the whole reply
    # "tts_timeout_ms": 120000,       # Kokoro synthesizing one sentence
}
