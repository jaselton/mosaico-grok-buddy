// SPDX-License-Identifier: Apache-2.0
#include "app_config.h"
#include "app_display.h"
#include "app_buddy.h"
#include "app_alarm.h"
#include "app_clock.h"
#include "app_motion.h"
#include "app_voice.h"
#include "app_workout.h"
#include "app_camera.h"
#include "bsp/display.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_iris.h"
#include "settings.h"
#include "bsp/button.h"
#include "freertos/FreeRTOS.h"
#include <stdatomic.h>
#include "esp_log.h"
#include "iris_ota_support.h"
#include "iris_screen_mirror.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ui_bundle.h"

static const char *TAG = "grok_desk_buddy";

#include "esp_heap_caps.h"
static void heap_mark(const char *where)
{
    ESP_LOGI(TAG, "heap %s: internal free %u, largest DMA block %u (display needs headroom)", where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
}

/* The composition root retains owned handles, never controller internals. */
static struct {
    esp_gsp_handle_t ui;
    app_buddy_handle_t controller;
} application;

/* Face-down puts the voice to sleep too (mic off); picking it up stops an alarm. */
static void on_sleep(bool asleep, void *ctx)
{
    (void)ctx;
    if (!asleep) {
        app_alarm_dismiss();
    }
    app_voice_set_asleep(asleep);
}

/* Shaking stops a ringing alarm (the face goes dizzy either way). */
static void on_shake(void *ctx)
{
    (void)ctx;
    app_alarm_dismiss();
}

/* Alarm ringing: worried eyes, buzz pattern and chimes until dismissed. */
static void on_alarm(bool ringing, const char *label, void *ctx)
{
    (void)ctx;
    app_buddy_set_alarm(application.controller, ringing);
    app_motion_set_alarm(ringing);
    app_voice_set_alarm(ringing, label);
}

/* Petting (long press): the face reacts itself; add a purr. */
static uint16_t s_scene;   /* the scene showing (any) */

static void on_call(uint16_t scene_id, uint16_t action, int32_t value, void *ctx)
{
    (void)ctx;
    settings_on_call(scene_id, action, value);
}

/* Swiping between the eyes, the world-timer and the countdown timer. */
static void on_scene(uint16_t scene_id, void *ctx)
{
    (void)ctx;
    s_scene = scene_id;
    app_clock_scene_changed(scene_id);
    settings_scene_changed(scene_id);
}

/* Beeps for the workout timer; Grok starting one also brings its screen up. */
static void on_workout(app_workout_event_t ev, int arg, void *ctx)
{
    (void)ctx;
    switch (ev) {
    case APP_WORKOUT_EV_START:
        app_voice_beep(APP_VOICE_BEEP_START);
        break;
    case APP_WORKOUT_EV_COUNT:
        app_voice_beep(APP_VOICE_BEEP_COUNT);
        break;
    case APP_WORKOUT_EV_PHASE:
        app_voice_beep(arg == APP_WORKOUT_REST ? APP_VOICE_BEEP_REST : APP_VOICE_BEEP_WORK);
        break;
    case APP_WORKOUT_EV_DONE:
        app_voice_beep(APP_VOICE_BEEP_DONE);
        break;
    case APP_WORKOUT_EV_STOP:
        break;
    }
}

static app_buddy_screen_ids_t s_ids;

static void on_show_screen(app_voice_screen_t scr, void *ctx)
{
    (void)ctx;
    uint16_t scene;
    switch (scr) {
    case APP_VOICE_SCREEN_WORKOUT: scene = s_ids.workout_scene; break;
    case APP_VOICE_SCREEN_EYES: scene = s_ids.buddy_scene; break;
    case APP_VOICE_SCREEN_CLOCK: scene = s_ids.clock_scene; break;
    case APP_VOICE_SCREEN_WEATHER: scene = s_ids.weather_scene; break;
    case APP_VOICE_SCREEN_FORECAST: scene = s_ids.forecast_scene; break;
    case APP_VOICE_SCREEN_CALENDAR: scene = s_ids.cal_scene; break;
    case APP_VOICE_SCREEN_USAGE: scene = s_ids.usage_scene; break;
    case APP_VOICE_SCREEN_TIMER:
    default: scene = s_ids.timer_scene; break;
    }
    esp_gsp_goto_scene(application.ui, scene, ESP_GSP_CROSS_FADE);
}

/* AI button (GPIO7): press = tap-to-talk, double press = next screen, long
 * press = stop everything (alarm, timer, workout, Grok). Its callbacks run on the
 * button timer task, so screen and workout work is handed to the UI task. */
static atomic_int s_btn_next, s_btn_stop;

static void on_ai_button(void *button, void *user_data)
{
    switch ((intptr_t)user_data) {
    case 0: app_voice_tap(); break;
    case 1: atomic_store(&s_btn_next, 1); break;
    default: atomic_store(&s_btn_stop, 1); break;
    }
}

static void button_tick(esp_gsp_handle_t ui, void *ctx)
{
    (void)ctx;
    if (atomic_exchange(&s_btn_next, 0) && !settings_close()) {
        const uint16_t order[] = { s_ids.buddy_scene, s_ids.clock_scene, s_ids.cal_scene, s_ids.weather_scene,
                                   s_ids.forecast_scene, s_ids.timer_scene, s_ids.workout_scene, s_ids.usage_scene };
        const int n = (int)(sizeof(order) / sizeof(order[0]));
        int i = 0;
        while (i < n && order[i] != s_scene) {
            ++i;
        }
        esp_gsp_goto_scene(ui, order[(i + 1) % n], ESP_GSP_CROSS_FADE);
    }
    if (atomic_exchange(&s_btn_stop, 0)) {
        app_alarm_dismiss();
        if (app_workout_active()) {
            app_workout_stop();
        }
        app_alarm_timer_t tm;
        if (app_alarm_timer_get(&tm) && tm.running) {
            app_alarm_timer_reset();
        }
        app_voice_stop();
        app_voice_beep(APP_VOICE_BEEP_DONE);
    }
}

static void start_ai_button(void)
{
    button_handle_t btn[BSP_BUTTON_NUM] = { 0 };
    int n = 0;
    if (bsp_iot_button_create(btn, &n, BSP_BUTTON_NUM) != ESP_OK || !btn[BSP_BUTTON_AI]) {
        ESP_LOGW(TAG, "AI button unavailable");
        return;
    }
    iot_button_register_cb(btn[BSP_BUTTON_AI], BUTTON_SINGLE_CLICK, NULL, on_ai_button, (void *)0);
    iot_button_register_cb(btn[BSP_BUTTON_AI], BUTTON_DOUBLE_CLICK, NULL, on_ai_button, (void *)1);
    iot_button_register_cb(btn[BSP_BUTTON_AI], BUTTON_LONG_PRESS_START, NULL, on_ai_button, (void *)2);
    esp_gsp_timer_create(application.ui, 50, button_tick, NULL);
    ESP_LOGI(TAG, "AI button: press talk, double next screen, hold stop");
}

/* Usage warning: a worried look when it first appears (no text under the eyes). */
static void on_usage_note(const char *note)
{
    static char last[64];
    if (strcmp(last, note) != 0) {
        if (note[0] && !last[0]) {
            app_buddy_express(application.controller, APP_BUDDY_EXPR_SAD);
        }
        snprintf(last, sizeof(last), "%s", note);
    }
}

/* Night mode 22:00-06:00: dim screen, sleepy face, no wake word, messages
 * held. Not while a workout, timer or alarm is going. Runs on the UI task. */
#define NIGHT_START_H 22
#define NIGHT_END_H   6
#define NIGHT_BRIGHTNESS 12

static void night_tick(esp_gsp_handle_t ui, void *ctx)
{
    (void)ui;
    (void)ctx;
    static int shown = -1;
    bool night = false;
    if (app_alarm_time_valid()) {
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        night = settings_prefs()->night_dim && (lt.tm_hour >= NIGHT_START_H || lt.tm_hour < NIGHT_END_H);
    }
    app_alarm_timer_t tm;
    if (app_workout_active() || app_alarm_ringing() ||
            (app_alarm_timer_get(&tm) && tm.running)) {
        night = false;
    }
    app_voice_set_night(night);
    const bool dim = night && app_voice_night_active();
    if (dim != shown) {
        shown = dim;
        bsp_display_brightness_set(dim ? NIGHT_BRIGHTNESS : settings_prefs()->brightness);
        app_buddy_set_sleep(application.controller, dim);
        ESP_LOGI(TAG, "night %s", dim ? "dim" : "bright");
    }
}

static void on_pet(void *ctx)
{
    (void)ctx;
    app_motion_purr();
}


void app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    ESP_ERROR_CHECK(nvs_flash_init());
    /* Recovery must remain reachable even when the UI bundle cannot be loaded. */
    iris_ota_support_start();

    esp_gsp_config_t config;
    const esp_err_t err = ui_bundle_open(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GSP UI unavailable (0x%x); Recovery RPC remains active", err);
        return;
    }
    esp_gsp_esp_lcd_config_t display;
    const esp_err_t display_err = app_display_init(&display);
    if (display_err != ESP_OK) {
        ESP_LOGE(TAG, "Display unavailable (0x%x); Recovery RPC remains active", display_err);
        return;
    }
    ESP_ERROR_CHECK(iris_screen_mirror_init());
    ESP_ERROR_CHECK(esp_gsp_esp_lcd_start(&config, &display, &application.ui));
    ESP_ERROR_CHECK(iris_screen_mirror_attach(application.ui));
    ESP_ERROR_CHECK(app_buddy_create(application.ui, &application.controller));
    /* GSP retains the callback context. Device UI/controller live until reboot;
     * teardown requires quiescing GSP before app_buddy_destroy(), then closing UI. */
    /* Wi-Fi and the xAI key come from Vibe Mode shared settings; app_voice owns them. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_workout_init(on_workout, NULL));
    const app_voice_config_t voice = { .buddy = application.controller };
    const esp_err_t voice_err = app_voice_start(&voice);
    if (voice_err == ESP_OK) {
        /* Needs Wi-Fi/netif from app_voice for SNTP time. */
        ESP_ERROR_CHECK_WITHOUT_ABORT(app_alarm_start(on_alarm, NULL));
    }
    const app_motion_config_t motion = {
        .buddy = application.controller,
        .on_sleep = voice_err == ESP_OK ? on_sleep : NULL,
        .on_shake = on_shake,
    };
    if (app_motion_start(&motion) == ESP_OK) {
        app_buddy_set_pet_handler(application.controller, on_pet, NULL);
    }
    app_buddy_screen_ids_t ids;
    app_buddy_screen_ids(&ids);
    s_ids = ids;
    app_clock_ids_t clock_ids = {
        .clock_scene = ids.clock_scene, .clock_canvas = ids.clock_canvas,
        .timer_scene = ids.timer_scene, .timer_canvas = ids.timer_canvas,
        .timer_digits = ids.timer_digits, .timer_sub = ids.timer_sub, .timer_reset = ids.timer_reset,
        .usage_scene = ids.usage_scene, .usage_canvas = ids.usage_canvas, .usage_title = ids.usage_title,
        .usage_money = ids.usage_money, .usage_msub = ids.usage_msub, .usage_week = ids.usage_week,
        .usage_voice = ids.usage_voice, .usage_upd = ids.usage_upd,
        .workout_scene = ids.workout_scene, .workout_canvas = ids.workout_canvas,
        .workout_title = ids.workout_title, .workout_phase = ids.workout_phase,
        .workout_digits = ids.workout_digits, .workout_round = ids.workout_round, .workout_btn = ids.workout_btn,
        .weather_scene = ids.weather_scene, .weather_canvas = ids.weather_canvas, .weather_temp = ids.weather_temp,
        .weather_cond = ids.weather_cond, .weather_upd = ids.weather_upd,
        .weather_pollen = ids.weather_pollen,
    };
    memcpy(clock_ids.weather_hour, ids.weather_hour, sizeof(ids.weather_hour));
    memcpy(clock_ids.weather_htemp, ids.weather_htemp, sizeof(ids.weather_htemp));
    clock_ids.forecast_scene = ids.forecast_scene;
    clock_ids.forecast_canvas = ids.forecast_canvas;
    clock_ids.forecast_title = ids.forecast_title;
    clock_ids.forecast_upd = ids.forecast_upd;
    memcpy(clock_ids.forecast_day, ids.forecast_day, sizeof(ids.forecast_day));
    memcpy(clock_ids.forecast_pop, ids.forecast_pop, sizeof(ids.forecast_pop));
    memcpy(clock_ids.forecast_lo, ids.forecast_lo, sizeof(ids.forecast_lo));
    memcpy(clock_ids.forecast_hi, ids.forecast_hi, sizeof(ids.forecast_hi));
    clock_ids.cal_scene = ids.cal_scene;
    clock_ids.cal_canvas = ids.cal_canvas;
    clock_ids.cal_date = ids.cal_date;
    clock_ids.cal_allday = ids.cal_allday;
    clock_ids.cal_when = ids.cal_when;
    clock_ids.cal_title = ids.cal_title;
    clock_ids.cal_sub = ids.cal_sub;
    clock_ids.cal_upd = ids.cal_upd;
    memcpy(clock_ids.cal_rt, ids.cal_rt, sizeof(ids.cal_rt));
    memcpy(clock_ids.cal_rn, ids.cal_rn, sizeof(ids.cal_rn));
    if (app_clock_start(application.ui, &clock_ids) == ESP_OK) {
        app_buddy_set_scene_handler(application.controller, on_scene, NULL);
        app_buddy_set_call_handler(application.controller, on_call, NULL);
        app_clock_set_usage_note_handler(on_usage_note);
    }
    if (voice_err == ESP_OK) {
        app_voice_set_screen_handler(on_show_screen, NULL);
        start_ai_button();
        settings_start(application.ui, &s_ids);
        esp_gsp_timer_create(application.ui, 2000, night_tick, NULL);
    }
    heap_mark("ready");
    /* Last, so the camera probe's short-lived tasks don't split the DMA heap. */
    if (app_camera_init() != ESP_OK) {
        ESP_LOGW(TAG, "camera support unavailable");
    }
    heap_mark("after camera probe");
    if (voice_err == ESP_OK) {
        ESP_LOGI(TAG, "Grok desk buddy ready (tap to talk)");
    } else {
        ESP_LOGW(TAG, "Grok voice unavailable (%s); tap cycles face states",
                 esp_err_to_name(voice_err));
    }
}
