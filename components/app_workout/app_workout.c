// SPDX-License-Identifier: Apache-2.0
#include "app_workout.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "app_workout";

#define TICK_US      (50 * 1000)
#define DONE_IDLE_S  (10 * 60)    /* a finished workout goes back to idle after this */

typedef struct {
    const char *key, *name;
    app_workout_plan_t plan;
} preset_def_t;

static const preset_def_t PRESETS[APP_WORKOUT_PRESET_COUNT] = {
    [APP_WORKOUT_TABATA]    = { "tabata",    "TABATA",    { 20, 10, 8 } },
    [APP_WORKOUT_EMOM]      = { "emom",      "EMOM",      { 60, 0, 10 } },
    [APP_WORKOUT_INTERVAL]  = { "interval",  "INTERVALS", { 40, 20, 10 } },
    [APP_WORKOUT_ROUNDS]    = { "rounds",    "ROUNDS",    { 180, 60, 5 } },
    [APP_WORKOUT_STOPWATCH] = { "stopwatch", "STOPWATCH", { 0, 0, 0 } },
    [APP_WORKOUT_CUSTOM]    = { "custom",    "CUSTOM",    { 45, 15, 6 } },
};

static struct {
    SemaphoreHandle_t lock;
    esp_timer_handle_t timer;
    app_workout_cb_t cb;
    void *ctx;
    app_workout_preset_t preset;
    app_workout_plan_t plans[APP_WORKOUT_PRESET_COUNT];   /* rounds adjustable per preset */
    bool started, paused, done;
    int64_t start_us;          /* when the lead-in began */
    int64_t paused_at_us;
    int64_t paused_total_us;
    int64_t done_at_us;
    /* last emitted, to detect edges */
    app_workout_phase_t last_phase;
    int last_round, last_count;
} w;

static int total_of(const app_workout_plan_t *p)
{
    if (p->rounds <= 0 || p->work_s <= 0) {
        return 0;
    }
    return p->rounds * p->work_s + (p->rounds - 1) * p->rest_s;
}

/* Compute the state at `now` (lock held). */
static void compute(int64_t now, app_workout_state_t *o)
{
    memset(o, 0, sizeof(*o));
    o->preset = w.preset;
    o->plan = w.plans[w.preset];
    snprintf(o->name, sizeof(o->name), "%s", PRESETS[w.preset].name);
    o->total_s = total_of(&o->plan);
    o->paused = w.paused;
    if (!w.started) {
        o->phase = w.done ? APP_WORKOUT_DONE : APP_WORKOUT_IDLE;
        o->round = w.done ? o->plan.rounds : 0;
        o->phase_left_ms = 0;
        o->elapsed_s = w.done ? o->total_s : 0;
        return;
    }
    const int64_t at = w.paused ? w.paused_at_us : now;
    const int64_t ms = (at - w.start_us - w.paused_total_us) / 1000;
    const int64_t prep_ms = APP_WORKOUT_PREP_S * 1000;
    if (ms < prep_ms) {
        o->phase = APP_WORKOUT_PREP;
        o->phase_left_ms = (int)(prep_ms - ms);
        o->phase_len_s = APP_WORKOUT_PREP_S;
        return;
    }
    const int64_t t = ms - prep_ms;
    o->elapsed_s = (int)(t / 1000);
    const app_workout_plan_t *p = &o->plan;
    if (o->total_s == 0) {   /* stopwatch: count up */
        o->phase = APP_WORKOUT_WORK;
        o->round = 1;
        o->phase_left_ms = (int)(t > 0x7fffffff ? 0x7fffffff : t);
        return;
    }
    if (t >= (int64_t)o->total_s * 1000) {
        o->phase = APP_WORKOUT_DONE;
        o->round = p->rounds;
        o->elapsed_s = o->total_s;
        return;
    }
    const int64_t cycle = (int64_t)(p->work_s + p->rest_s) * 1000;
    const int r = (int)(t / cycle);
    const int64_t within = t - r * cycle;
    o->round = r + 1;
    if (within < (int64_t)p->work_s * 1000) {
        o->phase = APP_WORKOUT_WORK;
        o->phase_len_s = p->work_s;
        o->phase_left_ms = (int)((int64_t)p->work_s * 1000 - within);
    } else {
        o->phase = APP_WORKOUT_REST;
        o->phase_len_s = p->rest_s;
        o->phase_left_ms = (int)(cycle - within);
    }
}

static void emit(app_workout_event_t ev, int arg)
{
    if (w.cb) {
        w.cb(ev, arg, w.ctx);
    }
}

static void tick(void *arg)
{
    (void)arg;
    app_workout_state_t st;
    int ev_phase = -1, ev_count = 0;
    bool ev_done = false;
    xSemaphoreTake(w.lock, portMAX_DELAY);
    const int64_t now = esp_timer_get_time();
    if (!w.started) {
        if (w.done && now - w.done_at_us > (int64_t)DONE_IDLE_S * 1000000) {
            w.done = false;
        }
        xSemaphoreGive(w.lock);
        return;
    }
    compute(now, &st);
    if (!w.paused) {
        if (st.phase == APP_WORKOUT_DONE) {
            w.started = false;
            w.done = true;
            w.done_at_us = now;
            ev_done = true;
        } else if (st.phase != w.last_phase || st.round != w.last_round) {
            if (st.phase == APP_WORKOUT_WORK || st.phase == APP_WORKOUT_REST) {
                ev_phase = st.phase;
            }
            w.last_count = 0;
        } else if (st.phase_len_s > 0) {
            const int left_s = (st.phase_left_ms + 999) / 1000;
            /* 3-2-1 before every change (skip for very short phases). */
            if (left_s >= 1 && left_s <= 3 && st.phase_len_s >= 5 && left_s != w.last_count) {
                ev_count = left_s;
                w.last_count = left_s;
            }
        }
        w.last_phase = st.phase;
        w.last_round = st.round;
    }
    xSemaphoreGive(w.lock);
    if (ev_done) {
        esp_timer_stop(w.timer);
        ESP_LOGI(TAG, "workout done");
        emit(APP_WORKOUT_EV_DONE, 0);
    } else if (ev_phase >= 0) {
        emit(APP_WORKOUT_EV_PHASE, ev_phase);
    } else if (ev_count) {
        emit(APP_WORKOUT_EV_COUNT, ev_count);
    }
}

esp_err_t app_workout_init(app_workout_cb_t cb, void *ctx)
{
    w.cb = cb;
    w.ctx = ctx;
    for (int i = 0; i < APP_WORKOUT_PRESET_COUNT; ++i) {
        w.plans[i] = PRESETS[i].plan;
    }
    w.preset = APP_WORKOUT_TABATA;
    w.lock = xSemaphoreCreateMutex();
    if (!w.lock) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t args = { .callback = tick, .name = "workout" };
    return esp_timer_create(&args, &w.timer);
}

static void reset_locked(void)
{
    w.started = w.paused = w.done = false;
    w.paused_total_us = 0;
    w.last_phase = APP_WORKOUT_IDLE;
    w.last_round = 0;
    w.last_count = 0;
}

void app_workout_select(app_workout_preset_t preset)
{
    if (preset < 0 || preset >= APP_WORKOUT_PRESET_COUNT || !w.lock) {
        return;
    }
    const bool was = app_workout_active();
    xSemaphoreTake(w.lock, portMAX_DELAY);
    reset_locked();
    w.preset = preset;
    xSemaphoreGive(w.lock);
    esp_timer_stop(w.timer);
    if (was) {
        emit(APP_WORKOUT_EV_STOP, 0);
    }
}

void app_workout_next_preset(void)
{
    app_workout_select((app_workout_preset_t)((w.preset + 1) % APP_WORKOUT_PRESET_COUNT));
}

esp_err_t app_workout_set_custom(const app_workout_plan_t *p)
{
    if (!p || p->work_s < 5 || p->work_s > 99 * 60 || p->rest_s < 0 || p->rest_s > 99 * 60 ||
            p->rounds < 1 || p->rounds > 99) {
        return ESP_ERR_INVALID_ARG;
    }
    w.plans[APP_WORKOUT_CUSTOM] = *p;
    app_workout_select(APP_WORKOUT_CUSTOM);
    return ESP_OK;
}

void app_workout_adjust_rounds(int delta)
{
    if (!w.lock) {
        return;
    }
    xSemaphoreTake(w.lock, portMAX_DELAY);
    app_workout_plan_t *p = &w.plans[w.preset];
    if (!w.started && p->rounds > 0) {
        int r = p->rounds + delta;
        p->rounds = r < 1 ? 1 : (r > 99 ? 99 : r);
        w.done = false;
    }
    xSemaphoreGive(w.lock);
}

void app_workout_start(void)
{
    if (!w.lock) {
        return;
    }
    xSemaphoreTake(w.lock, portMAX_DELAY);
    reset_locked();
    w.started = true;
    w.start_us = esp_timer_get_time();
    w.last_phase = APP_WORKOUT_PREP;
    xSemaphoreGive(w.lock);
    esp_timer_stop(w.timer);
    esp_timer_start_periodic(w.timer, TICK_US);
    ESP_LOGI(TAG, "start %s %d/%d x%d", PRESETS[w.preset].key, w.plans[w.preset].work_s,
             w.plans[w.preset].rest_s, w.plans[w.preset].rounds);
    emit(APP_WORKOUT_EV_START, 0);
}

void app_workout_pause(void)
{
    if (!w.lock) {
        return;
    }
    xSemaphoreTake(w.lock, portMAX_DELAY);
    if (w.started && !w.paused) {
        w.paused = true;
        w.paused_at_us = esp_timer_get_time();
    }
    xSemaphoreGive(w.lock);
}

void app_workout_resume(void)
{
    if (!w.lock) {
        return;
    }
    xSemaphoreTake(w.lock, portMAX_DELAY);
    if (w.started && w.paused) {
        w.paused_total_us += esp_timer_get_time() - w.paused_at_us;
        w.paused = false;
        w.last_count = 0;
    }
    xSemaphoreGive(w.lock);
}

void app_workout_toggle(void)
{
    app_workout_state_t st;
    app_workout_get(&st);
    if (st.phase == APP_WORKOUT_IDLE || st.phase == APP_WORKOUT_DONE) {
        app_workout_start();
    } else if (st.paused) {
        app_workout_resume();
    } else {
        app_workout_pause();
    }
}

void app_workout_stop(void)
{
    app_workout_select(w.preset);
}

void app_workout_get(app_workout_state_t *out)
{
    if (!w.lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(w.lock, portMAX_DELAY);
    compute(esp_timer_get_time(), out);
    xSemaphoreGive(w.lock);
}

bool app_workout_active(void)
{
    return w.started;
}

int app_workout_preset_from_name(const char *name)
{
    if (!name) {
        return -1;
    }
    for (int i = 0; i < APP_WORKOUT_PRESET_COUNT; ++i) {
        if (!strcasecmp(name, PRESETS[i].key) || !strcasecmp(name, PRESETS[i].name)) {
            return i;
        }
    }
    if (!strcasecmp(name, "intervals")) {
        return APP_WORKOUT_INTERVAL;
    }
    if (!strcasecmp(name, "boxing")) {
        return APP_WORKOUT_ROUNDS;
    }
    if (!strcasecmp(name, "for_time") || !strcasecmp(name, "clock")) {
        return APP_WORKOUT_STOPWATCH;
    }
    return -1;
}
