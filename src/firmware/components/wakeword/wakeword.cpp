// Detector implementation. Two pieces, both ported from ESPHome's micro_wake_word component
// (github.com/esphome/esphome, Apache-2.0) via the bare-ESP-IDF reference at
// github.com/0xD34D/micro_wake_word_standalone — see models/ATTRIBUTION.md:
//   1. The audio frontend (ESPMicroSpeechFeatures, a git dependency — see idf_component.yml):
//      converts 16 kHz PCM into 40-value log-mel feature slices, one per 10 ms step.
//   2. The streaming TFLite-Micro model: a stateful classifier that consumes feature slices and
//      emits a wake-word probability every `stride` slices (a model-embedded constant, 3 here —
//      i.e. one inference per 30 ms). A sliding window over the last few probabilities smooths the
//      decision (WAKEWORD_SLIDING_WINDOW_SIZE).
//
// Unlike the ESPHome original (an async Component with its own state machine, ring buffer, and
// microphone abstraction), this is a synchronous library: the mic-read loop is owned elsewhere,
// so wakeword_feed() just processes whatever PCM it's given and returns whether the wake word
// fired. This project only ever runs one wake-word model at a time,
// so ESPHome's StreamingModel/WakeWordModel/VADModel class hierarchy collapses to flat state
// below rather than being ported as-is.

#include "wakeword.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <memory>

#include "frontend.h"
#include "frontend_util.h"

#include "tensorflow/lite/core/c/common.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "wakeword";

// Embedded model (models/hey_billy.tflite, EMBED_FILES in CMakeLists.txt). ESP-IDF derives this
// symbol name from the file's basename ('.' -> '_', objcopy _binary_..._start convention).
// tflite::GetModel() only needs the start pointer — flatbuffers is self-describing.
extern const uint8_t hey_billy_tflite_start[] asm("_binary_hey_billy_tflite_start");

// Our own trained model — see models/ATTRIBUTION.md for training provenance. Cutoff chosen from
// the training run's ROC sweep (5% false-reject rate, ~0.19 false accepts/hour on the validation
// set); sliding window matches the training/test harness's own default (test.py,
// sliding_window_length=5). Swap these together with the model if it's ever retrained.
static constexpr float WAKEWORD_PROBABILITY_CUTOFF = 0.84f;
static constexpr size_t WAKEWORD_SLIDING_WINDOW_SIZE = 5;
// (feature_step_size, 10 ms, is baked into WAKEWORD_STEP_SAMPLES in wakeword.h.)

// Measured on-device: this model's interpreter actually uses 33540 bytes (init() logs
// arena_used_bytes() on every successful load); sized here with a ~9% margin — still trivial
// next to the N8R8's 8 MB PSRAM.
static constexpr size_t WAKEWORD_TENSOR_ARENA_SIZE = 36864;

// Frontend feature-slice shape — fixed by the model architecture, not the manifest.
static constexpr uint8_t FEATURE_SIZE = 40;      // features per 10 ms slice
static constexpr uint8_t FEATURE_WINDOW_MS = 30; // analysis window per slice (overlaps prior slices)

static constexpr uint32_t VARIABLE_ARENA_SIZE = 1024;
// Feature slices to process after (re)arming before accepting a detection (~740 ms at the 10 ms
// step) — lets the streaming model's internal state fill and the sliding window populate with
// real inferences before any detection can fire, matching the upstream MIN_SLICES_BEFORE_DETECTION.
static constexpr int16_t SLICES_BEFORE_DETECTION = 74;

static tflite::MicroMutableOpResolver<20> s_op_resolver;
static FrontendConfig s_frontend_config;
static FrontendState s_frontend_state;

static uint8_t *s_tensor_arena;
static uint8_t *s_var_arena;
static tflite::MicroResourceVariables *s_resource_vars;
static std::unique_ptr<tflite::MicroInterpreter> s_interpreter;
static uint8_t s_model_stride;     // feature slices per inference, read from the model's input tensor shape

static uint8_t s_current_stride_step;
static uint8_t s_recent_probabilities[WAKEWORD_SLIDING_WINDOW_SIZE];
static size_t s_last_n_index;
static int16_t s_ignore_windows;

static bool s_ready;   // wakeword_init() succeeded; feed()/reset() are no-ops otherwise

// The 20 TFLM ops a microWakeWord v2 streaming model can reference; every op must register
// successfully or the model's graph can't build.
static bool register_streaming_ops(tflite::MicroMutableOpResolver<20> &op_resolver)
{
    return op_resolver.AddCallOnce() == kTfLiteOk
        && op_resolver.AddVarHandle() == kTfLiteOk
        && op_resolver.AddReshape() == kTfLiteOk
        && op_resolver.AddReadVariable() == kTfLiteOk
        && op_resolver.AddStridedSlice() == kTfLiteOk
        && op_resolver.AddConcatenation() == kTfLiteOk
        && op_resolver.AddAssignVariable() == kTfLiteOk
        && op_resolver.AddConv2D() == kTfLiteOk
        && op_resolver.AddMul() == kTfLiteOk
        && op_resolver.AddAdd() == kTfLiteOk
        && op_resolver.AddMean() == kTfLiteOk
        && op_resolver.AddFullyConnected() == kTfLiteOk
        && op_resolver.AddLogistic() == kTfLiteOk
        && op_resolver.AddQuantize() == kTfLiteOk
        && op_resolver.AddDepthwiseConv2D() == kTfLiteOk
        && op_resolver.AddAveragePool2D() == kTfLiteOk
        && op_resolver.AddMaxPool2D() == kTfLiteOk
        && op_resolver.AddPad() == kTfLiteOk
        && op_resolver.AddPack() == kTfLiteOk
        && op_resolver.AddSplitV() == kTfLiteOk;
}

bool wakeword_init(void)
{
    // Frontend params match the upstream micro_wake_word component's setup() exactly — these are
    // load-bearing (the model was trained against this exact feature pipeline).
    s_frontend_config.window.size_ms = FEATURE_WINDOW_MS;
    s_frontend_config.window.step_size_ms = WAKEWORD_STEP_SAMPLES * 1000 / 16000;
    s_frontend_config.filterbank.num_channels = FEATURE_SIZE;
    s_frontend_config.filterbank.lower_band_limit = 125.0f;
    s_frontend_config.filterbank.upper_band_limit = 7500.0f;
    s_frontend_config.noise_reduction.smoothing_bits = 10;
    s_frontend_config.noise_reduction.even_smoothing = 0.025f;
    s_frontend_config.noise_reduction.odd_smoothing = 0.06f;
    s_frontend_config.noise_reduction.min_signal_remaining = 0.05f;
    s_frontend_config.pcan_gain_control.enable_pcan = 1;
    s_frontend_config.pcan_gain_control.strength = 0.95f;
    s_frontend_config.pcan_gain_control.offset = 80.0f;
    s_frontend_config.pcan_gain_control.gain_bits = 21;
    s_frontend_config.log_scale.enable_log = 1;
    s_frontend_config.log_scale.scale_shift = 6;

    if (!FrontendPopulateState(&s_frontend_config, &s_frontend_state, 16000))
    {
        ESP_LOGE(TAG, "init: failed to populate the audio frontend state");
        return false;
    }

    if (!register_streaming_ops(s_op_resolver))
    {
        ESP_LOGE(TAG, "init: failed to register streaming-model ops");
        FrontendFreeStateContents(&s_frontend_state);
        return false;
    }

    s_tensor_arena = (uint8_t *) heap_caps_malloc(WAKEWORD_TENSOR_ARENA_SIZE, MALLOC_CAP_SPIRAM);
    s_var_arena = (uint8_t *) heap_caps_malloc(VARIABLE_ARENA_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_tensor_arena || !s_var_arena)
    {
        ESP_LOGE(TAG, "init: PSRAM alloc failed (tensor_arena=%p var_arena=%p)",
                 (void *) s_tensor_arena, (void *) s_var_arena);
        FrontendFreeStateContents(&s_frontend_state);
        return false;
    }

    tflite::MicroAllocator *ma = tflite::MicroAllocator::Create(s_var_arena, VARIABLE_ARENA_SIZE);
    s_resource_vars = tflite::MicroResourceVariables::Create(ma, 20);

    const tflite::Model *model = tflite::GetModel(hey_billy_tflite_start);
    if (model->version() != TFLITE_SCHEMA_VERSION)
    {
        ESP_LOGE(TAG, "init: model schema version mismatch (got %" PRIu32 ", want %d)",
                 model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }

    s_interpreter = std::make_unique<tflite::MicroInterpreter>(
        model, s_op_resolver, s_tensor_arena, WAKEWORD_TENSOR_ARENA_SIZE, s_resource_vars);
    if (s_interpreter->AllocateTensors() != kTfLiteOk)
    {
        ESP_LOGE(TAG, "init: AllocateTensors failed");
        return false;
    }

    TfLiteTensor *input = s_interpreter->input(0);
    if (input->dims->size != 3 || input->dims->data[0] != 1 || input->dims->data[2] != FEATURE_SIZE
        || input->type != kTfLiteInt8)
    {
        ESP_LOGE(TAG, "init: unexpected input tensor shape/type");
        return false;
    }
    s_model_stride = (uint8_t) input->dims->data[1];

    TfLiteTensor *output = s_interpreter->output(0);
    if (output->dims->size != 2 || output->dims->data[0] != 1 || output->dims->data[1] != 1
        || output->type != kTfLiteUInt8)
    {
        ESP_LOGE(TAG, "init: unexpected output tensor shape/type");
        return false;
    }

    ESP_LOGI(TAG, "init: \"Hey Billy\" model loaded — stride=%d slices/inference, arena_used=%u bytes",
             s_model_stride, (unsigned) s_interpreter->arena_used_bytes());

    s_ready = true;
    wakeword_reset();
    return true;
}

void wakeword_reset(void)
{
    s_current_stride_step = 0;
    s_last_n_index = 0;
    s_ignore_windows = -SLICES_BEFORE_DETECTION;
    for (auto &p : s_recent_probabilities) p = 0;
}

// Processes exactly one WAKEWORD_STEP_SAMPLES (10 ms) slice. Returns true if this slice's
// inference (if any) puts the sliding-window average over the cutoff.
static bool feed_one_step(const int16_t *pcm)
{
    size_t num_samples_read = 0;
    FrontendOutput out = FrontendProcessSamples(&s_frontend_state, pcm, WAKEWORD_STEP_SAMPLES, &num_samples_read);
    if (out.size != FEATURE_SIZE)
    {
        // Expected exactly twice, ever: the frontend's 30 ms analysis window (480 samples) fills
        // from our 10 ms (160-sample) feeds and only starts emitting once primed — the first two
        // feeds after boot land at 160/320 samples (< 480) and correctly produce nothing. The
        // window state is never reset after that, so every later feed keeps it topped up and this
        // never recurs — and it can't mask a detection either, since it's within the warm-up period
        // where detection is already suppressed. Not a warning; just log it for visibility.
        ESP_LOGD(TAG, "feed: frontend priming — produced %u/%d features this slice",
                 (unsigned) out.size, FEATURE_SIZE);
        return false;
    }

    // Scale the frontend's uint16 (~0..670) output into the int8 range the quantized model
    // expects. Formula and rounding constant are the model's training-time convention — not
    // arbitrary, and must match exactly or the model silently never fires.
    int8_t features[FEATURE_SIZE];
    for (size_t i = 0; i < out.size; i++)
    {
        int32_t value = ((int32_t) out.values[i] * 256 + 333) / 666;
        value -= 128;
        features[i] = (int8_t) std::clamp<int32_t>(value, -128, 127);
    }

    TfLiteTensor *input = s_interpreter->input(0);
    std::memmove(tflite::GetTensorData<int8_t>(input) + FEATURE_SIZE * s_current_stride_step,
                 features, FEATURE_SIZE);
    s_current_stride_step++;

    if (s_current_stride_step >= s_model_stride)
    {
        s_current_stride_step = 0;
        if (s_interpreter->Invoke() != kTfLiteOk)
        {
            ESP_LOGW(TAG, "feed: interpreter invoke failed");
        }
        else
        {
            TfLiteTensor *output = s_interpreter->output(0);
            s_last_n_index = (s_last_n_index + 1) % WAKEWORD_SLIDING_WINDOW_SIZE;
            s_recent_probabilities[s_last_n_index] = output->data.uint8[0];
        }
    }

    // Warm-up and detection check happen every slice (not gated on this slice having produced a
    // fresh inference) — matches upstream's per-slice update_model_probabilities_/detect_wake_words_
    // cadence, so the ~740 ms warm-up is 74 slices * 10 ms, not 74 inferences * 30 ms.
    s_ignore_windows = std::min<int16_t>(s_ignore_windows + 1, 0);
    if (s_ignore_windows < 0) return false;

    uint32_t sum = 0;
    for (uint8_t p : s_recent_probabilities) sum += p;
    float average = (float) sum / (255.0f * WAKEWORD_SLIDING_WINDOW_SIZE);
    return average > WAKEWORD_PROBABILITY_CUTOFF;
}

bool wakeword_feed(const int16_t *pcm, size_t n_samples)
{
    if (!s_ready) return false;

    if (n_samples % WAKEWORD_STEP_SAMPLES != 0)
    {
        ESP_LOGW(TAG, "feed: %u samples is not a multiple of the %d-sample step — dropping the remainder",
                 (unsigned) n_samples, WAKEWORD_STEP_SAMPLES);
    }

    bool detected = false;
    for (size_t offset = 0; offset + WAKEWORD_STEP_SAMPLES <= n_samples; offset += WAKEWORD_STEP_SAMPLES)
    {
        if (feed_one_step(pcm + offset)) detected = true;
    }
    return detected;
}
