`hey_jarvis.tflite` — pretrained microWakeWord v2 model, "Hey Jarvis" by Kevin Ahrendt.

Source: https://github.com/esphome/micro-wake-word-models/blob/main/models/v2/hey_jarvis.tflite
License: Apache-2.0 (https://github.com/esphome/micro-wake-word-models/blob/main/LICENSE)

Manifest (`hey_jarvis.json` in the same source directory) — keep these four in sync with
`WAKEWORD_*` constants in `wakeword.cpp` if the model is ever swapped:

```json
{
  "probability_cutoff": 0.97,
  "feature_step_size": 10,
  "sliding_window_size": 5,
  "tensor_arena_size": 22860
}
```

This is the step-2 placeholder pretrained word (proves the detector pipeline end-to-end). Step 3
swaps this file — and only this file plus the four manifest constants — for a trained "hey billy"
model; nothing else in this component changes.
