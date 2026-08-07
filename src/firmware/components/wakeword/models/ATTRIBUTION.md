`hey_billy.tflite` — our own trained microWakeWord v2 model for "hey billy", trained 2026-07-24.

Training framework: `OHF-Voice/micro-wake-word` (Apache-2.0). Positive samples generated with
`rhasspy/piper-sample-generator`
(TTS, 1000 samples, `en_US-libritts_r-medium` voice); negative data from the framework's
pre-built HuggingFace sets (`speech`, `dinner_party`, `no_speech`, `dinner_party_eval`) plus MIT
room-impulse-response and FMA background-noise augmentation of the positive samples. Trained on
`gpu-host` in an isolated, disposable `/opt/training` tree — not part of this repo or the
firmware build; only this `.tflite` file is a build input.

Manifest values — keep these in sync with the `WAKEWORD_*` constants in `wakeword.cpp` if the
model is ever retrained:

```json
{
  "probability_cutoff": 0.84,
  "feature_step_size": 10,
  "sliding_window_size": 5,
  "tensor_arena_size": 33540
}
```

`probability_cutoff` chosen from the training run's ROC sweep: 5% false-reject rate, ~0.19 false
accepts/hour on the (synthetic) validation set. `sliding_window_size` matches the training
framework's own test-harness default (`microwakeword/test.py`, `sliding_window_length=5`).
`tensor_arena_size` is measured on-device (`wakeword_init()`'s logged `arena_used_bytes()`), not
taken from any manifest — arena sizing is not portable across TFLM builds, or even between two
models on the same build. Always re-derive it empirically if the model is ever retrained.

Real-world false-accept/false-reject rates will very likely differ from the synthetic validation
numbers above — expect to retrain (more/better samples, reweighted negatives, retuned cutoff) if
bench testing shows the model is too trigger-happy or too hard to wake.
