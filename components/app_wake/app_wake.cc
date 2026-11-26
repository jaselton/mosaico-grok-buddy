// SPDX-License-Identifier: Apache-2.0
/*
 * On-device wake word ("Hey Grok") with a microWakeWord streaming model.
 * Features come from the TFLite Micro audio frontend configured exactly like
 * pymicro-features (the training feature generator): 30 ms windows every
 * 10 ms, 40 mel channels 125-7500 Hz, noise reduction, PCAN, log scale.
 * Every `stride` feature frames the quantized streaming model returns a
 * probability; the mean of the last WINDOW scores above CUTOFF fires once,
 * followed by a refractory period.
 */
#include "app_wake.h"

#include <atomic>
#include <cmath>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/micro/schema/schema_generated.h"

static const char *TAG = "app_wake";

extern const uint8_t model_start[] asm("_binary_wake_word_tflite_start");
extern const uint8_t model_end[] asm("_binary_wake_word_tflite_end");

namespace {

constexpr int kSampleRate = 16000;
constexpr int kFeatures = 40;
constexpr float kFeatureScale = 0.0390625f;     /* pymicro-features FLOAT32_SCALE */
constexpr size_t kArenaBytes = 96 * 1024;
constexpr size_t kVarArenaBytes = 2 * 1024;
constexpr int kWindow = 5;                      /* scores averaged */
constexpr float kCutoff = 0.90f;                /* tuned against the test results */
constexpr int kRefractoryMs = 2000;
constexpr size_t kRingBytes = kSampleRate * 2;  /* 1 s of PCM16 */

struct Wake {
    app_wake_cb_t cb = nullptr;
    void *ctx = nullptr;
    StreamBufferHandle_t ring = nullptr;
    std::atomic<bool> enabled{true};
    std::atomic<bool> resumed{false};   /* re-settle scores after a pause */
    FrontendState frontend{};
    tflite::MicroInterpreter *interp = nullptr;
    int stride = 3;
    float in_scale = 1.0f;
    int in_zero = 0;
};
Wake w;

bool frontend_init()
{
    FrontendConfig c;
    FrontendFillConfigWithDefaults(&c);
    c.window.size_ms = 30;
    c.window.step_size_ms = 10;
    c.filterbank.num_channels = kFeatures;
    c.filterbank.lower_band_limit = 125.0;
    c.filterbank.upper_band_limit = 7500.0;
    c.noise_reduction.smoothing_bits = 10;
    c.noise_reduction.even_smoothing = 0.025;
    c.noise_reduction.odd_smoothing = 0.06;
    c.noise_reduction.min_signal_remaining = 0.05;
    c.pcan_gain_control.enable_pcan = 1;
    c.pcan_gain_control.strength = 0.95;
    c.pcan_gain_control.offset = 80.0;
    c.pcan_gain_control.gain_bits = 21;
    c.log_scale.enable_log = 1;
    c.log_scale.scale_shift = 6;
    return FrontendPopulateState(&c, &w.frontend, kSampleRate) != 0;
}

bool model_init()
{
    const size_t len = (size_t)(model_end - model_start);
    if (len < 1024) {
        return false;   /* placeholder: no model built in */
    }
    const tflite::Model *model = tflite::GetModel(model_start);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "model schema %lu != %d", (unsigned long)model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }
    /* Ops used by microWakeWord streaming models. */
    static tflite::MicroMutableOpResolver<20> ops;
    ops.AddCallOnce();
    ops.AddVarHandle();
    ops.AddReshape();
    ops.AddReadVariable();
    ops.AddStridedSlice();
    ops.AddConcatenation();
    ops.AddAssignVariable();
    ops.AddConv2D();
    ops.AddMul();
    ops.AddAdd();
    ops.AddMean();
    ops.AddFullyConnected();
    ops.AddLogistic();
    ops.AddQuantize();
    ops.AddDepthwiseConv2D();
    ops.AddAveragePool2D();
    ops.AddMaxPool2D();
    ops.AddPad();
    ops.AddPack();
    ops.AddSplitV();

    uint8_t *arena = (uint8_t *)heap_caps_aligned_alloc(16, kArenaBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *var_arena = (uint8_t *)heap_caps_aligned_alloc(16, kVarArenaBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!arena || !var_arena) {
        return false;
    }
    tflite::MicroAllocator *va = tflite::MicroAllocator::Create(var_arena, kVarArenaBytes);
    tflite::MicroResourceVariables *vars = tflite::MicroResourceVariables::Create(va, 20);
    static tflite::MicroInterpreter interp(model, ops, arena, kArenaBytes, vars);
    if (interp.AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed (arena %u bytes)", (unsigned)kArenaBytes);
        return false;
    }
    TfLiteTensor *in = interp.input(0);
    if (in->type != kTfLiteInt8 || in->dims->size != 3 || in->dims->data[2] != kFeatures) {
        ESP_LOGE(TAG, "unexpected model input");
        return false;
    }
    w.stride = in->dims->data[1];
    w.in_scale = in->params.scale;
    w.in_zero = in->params.zero_point;
    w.interp = &interp;
    ESP_LOGI(TAG, "model %u bytes, stride %d, arena used %u", (unsigned)len, w.stride,
             (unsigned)interp.arena_used_bytes());
    return true;
}

int8_t quantize(uint16_t v)
{
    const float f = (float)v * kFeatureScale;
    long q = lroundf(f / w.in_scale) + w.in_zero;
    return (int8_t)(q < -128 ? -128 : (q > 127 ? 127 : q));
}

void wake_task(void *)
{
    int16_t pcm[160];
    float scores[kWindow] = {0};
    int score_pos = 0, frames = 0, ignore_scores = kWindow * 4;
    TickType_t refractory_until = 0;
    TfLiteTensor *in = w.interp->input(0);
    for (;;) {
        const size_t got = xStreamBufferReceive(w.ring, pcm, sizeof(pcm), portMAX_DELAY);
        size_t n = got / sizeof(int16_t), off = 0;
        while (off < n) {
            size_t read = 0;
            FrontendOutput o = FrontendProcessSamples(&w.frontend, pcm + off, n - off, &read);
            off += read;
            if (!o.size) {
                continue;
            }
            int8_t *dst = in->data.int8 + frames * kFeatures;
            for (int i = 0; i < kFeatures; ++i) {
                dst[i] = quantize(o.values[i]);
            }
            if (++frames < w.stride) {
                continue;
            }
            frames = 0;
            if (w.interp->Invoke() != kTfLiteOk) {
                continue;
            }
            if (w.resumed.exchange(false)) {
                ignore_scores = kWindow * 4;
                memset(scores, 0, sizeof(scores));
            }
            const float p = (float)w.interp->output(0)->data.uint8[0] / 255.0f;
            scores[score_pos] = p;
            score_pos = (score_pos + 1) % kWindow;
            if (ignore_scores > 0) {   /* let the model state settle after a pause */
                --ignore_scores;
                continue;
            }
            float mean = 0;
            for (float s : scores) {
                mean += s;
            }
            mean /= kWindow;
            if (mean >= kCutoff && xTaskGetTickCount() >= refractory_until && w.enabled.load()) {
                ESP_LOGI(TAG, "wake word (score %.2f)", mean);
                refractory_until = xTaskGetTickCount() + pdMS_TO_TICKS(kRefractoryMs);
                memset(scores, 0, sizeof(scores));
                if (w.cb) {
                    w.cb(w.ctx);
                }
            }
        }
    }
}

}  // namespace

extern "C" esp_err_t app_wake_start(app_wake_cb_t cb, void *ctx)
{
    w.cb = cb;
    w.ctx = ctx;
    if (!model_init()) {
        ESP_LOGW(TAG, "no wake-word model built in; tap to talk only");
        return ESP_ERR_NOT_FOUND;
    }
    if (!frontend_init()) {
        return ESP_ERR_NO_MEM;
    }
    uint8_t *storage = (uint8_t *)heap_caps_malloc(kRingBytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    auto *meta = (StaticStreamBuffer_t *)heap_caps_malloc(sizeof(StaticStreamBuffer_t), MALLOC_CAP_INTERNAL);
    if (!storage || !meta) {
        return ESP_ERR_NO_MEM;
    }
    w.ring = xStreamBufferCreateStatic(kRingBytes + 1, sizeof(int16_t), storage, meta);
    if (xTaskCreateWithCaps(wake_task, "wake", 6144, nullptr, 4, nullptr, MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "listening for the wake word");
    return ESP_OK;
}

extern "C" void app_wake_feed(const int16_t *pcm, size_t samples)
{
    if (w.ring && w.enabled.load()) {
        xStreamBufferSend(w.ring, pcm, samples * sizeof(int16_t), 0);
    }
}

extern "C" void app_wake_set_enabled(bool enabled)
{
    if (enabled && !w.enabled.exchange(true)) {
        w.resumed.store(true);
    } else if (!enabled) {
        w.enabled.store(false);
    }
}
