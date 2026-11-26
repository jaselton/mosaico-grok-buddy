// SPDX-License-Identifier: Apache-2.0
#pragma once
/*
 * Workout interval timer: presets (Tabata, EMOM, intervals, boxing rounds,
 * stopwatch) plus one custom plan from voice. A 10 s "get ready" lead-in, then
 * work/rest phases. Monotonic clock, so it runs before SNTP and survives the
 * screen being swiped away. Events (for beeps) come from an esp_timer task:
 * keep the callback short and non-blocking.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_WORKOUT_TABATA = 0,
    APP_WORKOUT_EMOM,
    APP_WORKOUT_INTERVAL,
    APP_WORKOUT_ROUNDS,
    APP_WORKOUT_STOPWATCH,
    APP_WORKOUT_CUSTOM,
    APP_WORKOUT_PRESET_COUNT,
} app_workout_preset_t;

typedef enum {
    APP_WORKOUT_IDLE = 0,   /* set up, not started */
    APP_WORKOUT_PREP,       /* get-ready countdown */
    APP_WORKOUT_WORK,
    APP_WORKOUT_REST,
    APP_WORKOUT_DONE,
} app_workout_phase_t;

typedef struct {
    int work_s;   /* work interval (EMOM: the minute); stopwatch: 0 */
    int rest_s;   /* 0 = none */
    int rounds;   /* stopwatch: 0 (counts up, no end) */
} app_workout_plan_t;

typedef struct {
    app_workout_preset_t preset;
    app_workout_plan_t plan;
    char name[16];              /* "TABATA", "EMOM", ... */
    app_workout_phase_t phase;
    bool paused;
    int round;                  /* 1-based, 0 before the first work phase */
    int phase_left_ms;          /* countdown within the phase (stopwatch: elapsed) */
    int phase_len_s;            /* length of the current phase (0 = open-ended) */
    int elapsed_s;              /* since the get-ready ended */
    int total_s;                /* whole workout without the lead-in (0 = open-ended) */
} app_workout_state_t;

typedef enum {
    APP_WORKOUT_EV_START,    /* get-ready began */
    APP_WORKOUT_EV_COUNT,    /* arg = 3, 2, 1 seconds left in the phase */
    APP_WORKOUT_EV_PHASE,    /* arg = new app_workout_phase_t (WORK/REST) */
    APP_WORKOUT_EV_DONE,
    APP_WORKOUT_EV_STOP,     /* stopped/reset by the user */
} app_workout_event_t;

typedef void (*app_workout_cb_t)(app_workout_event_t ev, int arg, void *ctx);

#define APP_WORKOUT_PREP_S 10

esp_err_t app_workout_init(app_workout_cb_t cb, void *ctx);
/** Choose a preset (only while idle/done; resets to idle). */
void app_workout_select(app_workout_preset_t preset);
/** Next preset in the list (touch: tap the title). */
void app_workout_next_preset(void);
/** Store a custom plan (voice) and select it. Returns ESP_ERR_INVALID_ARG if out of range. */
esp_err_t app_workout_set_custom(const app_workout_plan_t *plan);
/** Change the number of rounds of the selected plan while idle (1..99). */
void app_workout_adjust_rounds(int delta);
/** Start from the get-ready lead-in. */
void app_workout_start(void);
/** Idle/done: start. Running: pause. Paused: resume. */
void app_workout_toggle(void);
void app_workout_pause(void);
void app_workout_resume(void);
/** Stop and go back to idle with the same plan. */
void app_workout_stop(void);
void app_workout_get(app_workout_state_t *out);
/** True from start until done/stopped (paused counts as active). */
bool app_workout_active(void);
/** Parse a preset name ("tabata", "emom", "interval", "rounds", "stopwatch", "custom"); -1 if unknown. */
int app_workout_preset_from_name(const char *name);

#ifdef __cplusplus
}
#endif
