// SPDX-License-Identifier: Apache-2.0
#pragma once
/* Shared between the clock face and the timer face (one touch interceptor). */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_gsp.h"

esp_err_t timer_face_start(esp_gsp_handle_t ui, uint16_t scene, uint16_t canvas, uint16_t digits,
                           uint16_t sub, uint16_t reset);
void timer_face_scene_changed(uint16_t scene_id);
/** Touch while the timer screen shows; returns true to consume the sample. */
bool timer_face_input(int32_t x, int32_t y, bool pressed);
bool timer_face_active(void);
