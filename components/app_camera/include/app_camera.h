// SPDX-License-Identifier: Apache-2.0
#pragma once
/*
 * Optional camera module (left expansion slot). Everything works without it:
 * app_camera_present() is false and captures fail cleanly. The camera is only
 * powered and streaming for the duration of one capture.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start slot discovery (cheap; no camera streaming). Safe to call once at boot. */
esp_err_t app_camera_init(void);

/** True when a camera module is plugged into the left slot. */
bool app_camera_present(void);

/** Take one photo and return it as a JPEG (about 480x640, upright). The caller
 * frees *jpeg with heap_caps_free(). Blocks ~1 s. ESP_ERR_NOT_FOUND = no camera. */
esp_err_t app_camera_capture_jpeg(uint8_t **jpeg, size_t *len);

#ifdef __cplusplus
}
#endif
