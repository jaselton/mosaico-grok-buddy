// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_gsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/** World-timer clock face drawn into a GSP Canvas (480x480, RGB565).
 * Functions: local time; world time (24 h day/night disc follows UTC, tap a
 * city to make it home); a daily alarm on a central hand (drag to set, tap
 * ON/OFF to arm); battery as the alarm reserve; a chronograph (tap the top-right
 * corner to start/stop, bottom-right to reset); big date. */
esp_err_t app_clock_start(esp_gsp_handle_t ui, uint16_t canvas_bind, uint16_t scene_id);

/** Tell the clock which scene is showing (drives repaint and touch). */
void app_clock_scene_changed(uint16_t scene_id);

/** Minutes the chronograph has been running (0 if stopped/reset). */
uint32_t app_clock_chrono_seconds(void);

#ifdef __cplusplus
}
#endif
