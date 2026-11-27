// SPDX-License-Identifier: Apache-2.0
#pragma once
/* Shared between the clock face and the timer face (one touch interceptor). */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_gsp.h"
#include "app_clock.h"

esp_err_t timer_face_start(esp_gsp_handle_t ui, uint16_t scene, uint16_t canvas, uint16_t digits,
                           uint16_t sub, uint16_t reset);
void timer_face_scene_changed(uint16_t scene_id);
/** Touch while the timer screen shows; returns true to consume the sample. */
bool timer_face_input(int32_t x, int32_t y, bool pressed);
bool timer_face_active(void);

/* Usage screen (Grok team usage, voice minutes). */
typedef struct {
    uint16_t scene, canvas, title, money, msub, week, voice, upd;
} usage_face_ids_t;
esp_err_t usage_face_start(esp_gsp_handle_t ui, const usage_face_ids_t *ids);
void usage_face_scene_changed(uint16_t scene_id);
void usage_face_set_note_handler(app_clock_usage_note_cb_t cb);
/* Workout interval timer screen. */
typedef struct {
    uint16_t scene, canvas, title, phase, digits, round, btn;
} workout_face_ids_t;
esp_err_t workout_face_start(esp_gsp_handle_t ui, const workout_face_ids_t *ids);
void workout_face_scene_changed(uint16_t scene_id);
bool workout_face_input(int32_t x, int32_t y, bool pressed);
bool workout_face_active(void);
/* Weather screen (relay GET /weather). */
typedef struct {
    uint16_t scene, canvas, temp, cond, upd, pollen;
    uint16_t hour[5], htemp[5];
    uint16_t fscene, fcanvas, ftitle, fupd;
    uint16_t fday[5], fpop[5], flo[5], fhi[5];
} weather_face_ids_t;
esp_err_t weather_face_start(esp_gsp_handle_t ui, const weather_face_ids_t *ids);
/* Calendar screen (relay GET /calendar). */
typedef struct {
    uint16_t scene, canvas, date, allday, when, title, sub, upd;
    uint16_t rt[4], rn[4];
} calendar_face_ids_t;
typedef void (*calendar_next_cb_t)(const char *note);
esp_err_t calendar_face_start(esp_gsp_handle_t ui, const calendar_face_ids_t *ids);
void calendar_face_scene_changed(uint16_t scene_id);
void calendar_face_set_next_handler(calendar_next_cb_t cb);
void weather_face_scene_changed(uint16_t scene_id);
