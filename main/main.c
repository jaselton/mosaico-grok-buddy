// SPDX-License-Identifier: Apache-2.0
#include "app_config.h"
#include "app_display.h"
#include "app_buddy.h"
#include "app_alarm.h"
#include "app_clock.h"
#include "app_motion.h"
#include "app_voice.h"
#include "esp_iris.h"
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
/* Swiping between the eyes, the world-timer and the countdown timer. */
static void on_scene(uint16_t scene_id, void *ctx)
{
    (void)ctx;
    app_clock_scene_changed(scene_id);
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
    const app_clock_ids_t clock_ids = {
        .clock_scene = ids.clock_scene, .clock_canvas = ids.clock_canvas,
        .timer_scene = ids.timer_scene, .timer_canvas = ids.timer_canvas,
        .timer_digits = ids.timer_digits, .timer_sub = ids.timer_sub, .timer_reset = ids.timer_reset,
        .usage_scene = ids.usage_scene, .usage_canvas = ids.usage_canvas, .usage_title = ids.usage_title,
        .usage_money = ids.usage_money, .usage_msub = ids.usage_msub, .usage_week = ids.usage_week,
        .usage_voice = ids.usage_voice, .usage_upd = ids.usage_upd,
    };
    if (app_clock_start(application.ui, &clock_ids) == ESP_OK) {
        app_buddy_set_scene_handler(application.controller, on_scene, NULL);
    }
    heap_mark("ready");
    if (voice_err == ESP_OK) {
        ESP_LOGI(TAG, "Grok desk buddy ready (tap to talk)");
    } else {
        ESP_LOGW(TAG, "Grok voice unavailable (%s); tap cycles face states",
                 esp_err_to_name(voice_err));
    }
}
