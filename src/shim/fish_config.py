"""Runtime-tunable firmware constants, served over GET /v1/config so they can be retuned after
final assembly without a firmware rebuild+reflash.

OVERRIDES contains values intentionally supplied by the backend. The firmware has a compiled-in
default for every field (fish_config.c) and only overwrites fields present in the response. Empty
it to use all firmware defaults; add a key to change it on the fish's next config fetch.
"""

OVERRIDES = {
    # Every field below is commented out with the firmware's current compiled-in default
    # (fish_config.c) -- uncomment one to override it on the fish's next config fetch; re-comment
    # (or delete) to fall back to that default again. Keep the values shown here in sync with
    # fish_config.c by hand if that file's own defaults ever change.

    # VAD (voice-activity detection) -- audio.c audio_capture_utterance()
    # "vad_onset_rms": 6000,        # 24-bit scale: idle ~2000, speech >7000; also the min VAD threshold
    # "vad_noise_margin": 3000,     # added to measured ambient RMS for the effective VAD threshold
    # "vad_silence_ms": 600,        # end the turn after this much sub-threshold audio
    # "vad_min_voiced_ms": 250,     # reject a capture with less real speech than this (clicks)
    # "vad_drain_ms": 250,          # discard the mic's buffered prompt tone before listening
    # "capture_max_ms": 10000,      # hard cap on one utterance

    # Mouth lip-sync envelope -- hal.c's playback envelope follower
    # "mouth_env_ref": 6000.0,        # RMS that saturates the envelope at 1.0 (16-bit PCM scale)
    # "mouth_env_attack": 0.6,
    # "mouth_env_release": 0.15,
    # "mouth_mid_threshold": 0.3,     # envelope above this -> MID; below -> CLOSED
    # "mouth_open_threshold": 0.65,   # envelope above this -> OPEN (full duty); below -> MID
    # "mouth_mid_duty_pct": 80,       # mouth partial deflection for better speaking choreography

    # Tail choreography timing -- hal.c fish_hal_tail_flap()
    # "tail_flap_ms": 250,     # one flap: drive out, then let the spring return it
    # "tail_settle_ms": 350,   # spring-return travel + mechanical ring-down before it's safe to listen

    # Backend HTTP timeouts -- net.c
    # "stt_timeout_ms": 30000,        # whisper.cpp transcribing the whole utterance
    # "respond_timeout_ms": 120000,   # shim SSE stream: LLM generation across the whole reply
    # "tts_timeout_ms": 120000,       # Kokoro synthesizing one sentence

}
