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
typedef struct {
    uint16_t clock_scene, clock_canvas;
    uint16_t timer_scene, timer_canvas, timer_digits, timer_sub, timer_reset;
    uint16_t usage_scene, usage_canvas, usage_title, usage_money, usage_msub, usage_week, usage_voice, usage_upd;
    uint16_t workout_scene, workout_canvas, workout_title, workout_phase, workout_digits, workout_round, workout_btn;
    uint16_t weather_scene, weather_canvas, weather_temp, weather_cond, weather_upd, weather_pollen;
    uint16_t weather_hour[5], weather_htemp[5];
    uint16_t forecast_scene, forecast_canvas, forecast_title, forecast_upd;
    uint16_t forecast_day[5], forecast_pop[5], forecast_lo[5], forecast_hi[5];
    uint16_t cal_scene, cal_canvas, cal_date, cal_allday, cal_when, cal_title, cal_sub, cal_upd;
    uint16_t cal_rt[4], cal_rn[4];
} app_clock_ids_t;

/** Starts the world-timer, weather, countdown-timer, workout and usage screens. */
esp_err_t app_clock_start(esp_gsp_handle_t ui, const app_clock_ids_t *ids);

/** Tell the clock which scene is showing (drives repaint and touch). */
void app_clock_scene_changed(uint16_t scene_id);

/** Usage warning for the eyes screen ("" = none); runs on the UI task. */
typedef void (*app_clock_usage_note_cb_t)(const char *note);
void app_clock_set_usage_note_handler(app_clock_usage_note_cb_t cb);

/** Next calendar event for the eyes screen ("" = none soon); runs on the UI task. */
typedef void (*app_clock_next_cb_t)(const char *note);
void app_clock_set_next_event_handler(app_clock_next_cb_t cb);

/** Seconds the chronograph has been running (0 if stopped/reset). */
uint32_t app_clock_chrono_seconds(void);

#ifdef __cplusplus
}
#endif
