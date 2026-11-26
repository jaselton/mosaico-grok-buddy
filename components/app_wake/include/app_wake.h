// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Wake-word detection callback; runs on the wake task, must not block. */
typedef void (*app_wake_cb_t)(void *ctx);

/** Load the embedded microWakeWord model (wakeword/hi_grok.tflite) and start
 * detection. Returns ESP_ERR_NOT_FOUND when no model was built in. */
esp_err_t app_wake_start(app_wake_cb_t cb, void *ctx);

/** Feed 16 kHz mono PCM16 from any single task; dropped while disabled. */
void app_wake_feed(const int16_t *pcm, size_t samples);

/** Pause detection (e.g. while a conversation is running) or resume it. */
void app_wake_set_enabled(bool enabled);

#ifdef __cplusplus
}
#endif
