// SPDX-License-Identifier: Apache-2.0
/*
 * One-shot alarms for the desk buddy. The wall clock comes from SNTP (the
 * board has no battery-backed RTC), local time from CONFIG_GROK_BUDDY_TZ.
 * Alarms are stored as absolute epoch seconds in NVS so they survive a
 * reboot; one that came due while the board was off rings on boot only if
 * it is less than 10 minutes late. A ringing alarm stops on dismiss or after
 * RING_MAX_S, and reports start/stop through the callback.
 */
#include "app_alarm.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "app_alarm";

#define NVS_NS        "alarms"
#define VALID_EPOCH   1767225600LL   /* 2026-01-01: clock considered set after this */
#define LATE_GRACE_S  600
#define RING_MAX_S    300

typedef struct {
    int64_t when;                    /* epoch seconds; 0 = empty */
    char label[APP_ALARM_LABEL_MAX];
} alarm_t;

static struct {
    alarm_t alarms[APP_ALARM_MAX];
    SemaphoreHandle_t lock;
    app_alarm_ring_cb_t cb;
    void *ctx;
    atomic_bool ringing;
    atomic_bool dismiss;
    char ring_label[APP_ALARM_LABEL_MAX];
} a;

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, "list", a.alarms, sizeof(a.alarms));
    nvs_commit(h);
    nvs_close(h);
}

static void load(void)
{
    nvs_handle_t h;
    size_t len = sizeof(a.alarms);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "list", a.alarms, &len) != ESP_OK || len != sizeof(a.alarms)) {
            memset(a.alarms, 0, sizeof(a.alarms));
        }
        nvs_close(h);
    }
}

bool app_alarm_time_valid(void)
{
    return (int64_t)time(NULL) > VALID_EPOCH;
}

void app_alarm_format_now(char *out, size_t len)
{
    if (!app_alarm_time_valid()) {
        snprintf(out, len, "unknown (clock not set yet)");
        return;
    }
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    strftime(out, len, "%A, %B %d, %I:%M %p", &t);
}

static void describe(time_t when, char *out, size_t len)
{
    time_t now = time(NULL);
    struct tm tw, tn;
    localtime_r(&when, &tw);
    localtime_r(&now, &tn);
    char hm[16];
    strftime(hm, sizeof(hm), "%I:%M %p", &tw);
    const char *p = hm[0] == '0' ? hm + 1 : hm;   /* "06:30" -> "6:30" */
    const bool today = tw.tm_yday == tn.tm_yday && tw.tm_year == tn.tm_year;
    snprintf(out, len, "%s %s", p, today ? "today" : "tomorrow");
}

esp_err_t app_alarm_set(int hour, int minute, const char *label, char *out, size_t len)
{
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!app_alarm_time_valid()) {
        return ESP_ERR_INVALID_STATE;
    }
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    time_t when = mktime(&t);
    if (when <= now) {
        t.tm_mday += 1;
        t.tm_isdst = -1;
        when = mktime(&t);
    }
    xSemaphoreTake(a.lock, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < APP_ALARM_MAX; ++i) {
        if (!a.alarms[i].when || a.alarms[i].when == when) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        xSemaphoreGive(a.lock);
        return ESP_ERR_NO_MEM;
    }
    a.alarms[slot].when = when;
    snprintf(a.alarms[slot].label, sizeof(a.alarms[slot].label), "%s", label ? label : "");
    save();
    xSemaphoreGive(a.lock);
    describe(when, out, len);
    ESP_LOGI(TAG, "alarm set for %s", out);
    return ESP_OK;
}

int app_alarm_cancel_all(void)
{
    xSemaphoreTake(a.lock, portMAX_DELAY);
    int n = 0;
    for (int i = 0; i < APP_ALARM_MAX; ++i) {
        n += a.alarms[i].when != 0;
    }
    memset(a.alarms, 0, sizeof(a.alarms));
    save();
    xSemaphoreGive(a.lock);
    return n;
}

bool app_alarm_dismiss(void)
{
    if (!atomic_load(&a.ringing)) {
        return false;
    }
    atomic_store(&a.dismiss, true);
    return true;
}

bool app_alarm_ringing(void)
{
    return atomic_load(&a.ringing);
}

static void alarm_task(void *arg)
{
    (void)arg;
    bool booted = true;
    int64_t ring_started = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!app_alarm_time_valid()) {
            continue;
        }
        static bool logged;
        if (!logged) {
            logged = true;
            char buf[64];
            app_alarm_format_now(buf, sizeof(buf));
            ESP_LOGI(TAG, "clock synced: %s", buf);
        }
        const int64_t now = (int64_t)time(NULL);
        if (atomic_load(&a.ringing)) {
            if (atomic_exchange(&a.dismiss, false) || now - ring_started > RING_MAX_S) {
                atomic_store(&a.ringing, false);
                ESP_LOGI(TAG, "alarm stopped");
                if (a.cb) {
                    a.cb(false, a.ring_label, a.ctx);
                }
            }
            continue;
        }
        atomic_store(&a.dismiss, false);
        xSemaphoreTake(a.lock, portMAX_DELAY);
        int due = -1;
        bool changed = false;
        for (int i = 0; i < APP_ALARM_MAX; ++i) {
            if (!a.alarms[i].when || a.alarms[i].when > now) {
                continue;
            }
            if (booted && now - a.alarms[i].when > LATE_GRACE_S) {
                ESP_LOGW(TAG, "dropping alarm missed while off");
                a.alarms[i].when = 0;
                changed = true;
                continue;
            }
            due = i;
            break;
        }
        if (due >= 0) {
            snprintf(a.ring_label, sizeof(a.ring_label), "%s", a.alarms[due].label);
            a.alarms[due].when = 0;
            changed = true;
        }
        if (changed) {
            save();
        }
        xSemaphoreGive(a.lock);
        booted = false;
        if (due >= 0) {
            ring_started = now;
            atomic_store(&a.ringing, true);
            ESP_LOGI(TAG, "alarm ringing%s%s", a.ring_label[0] ? ": " : "", a.ring_label);
            if (a.cb) {
                a.cb(true, a.ring_label, a.ctx);
            }
        }
    }
}

esp_err_t app_alarm_start(app_alarm_ring_cb_t cb, void *ctx)
{
    a.cb = cb;
    a.ctx = ctx;
    a.lock = xSemaphoreCreateMutex();
    if (!a.lock) {
        return ESP_ERR_NO_MEM;
    }
    setenv("TZ", CONFIG_GROK_BUDDY_TZ, 1);
    tzset();
    load();
    /* SNTP syncs once the station gets an IP; netif must already be initialised. */
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp.start = true;
    esp_err_t err = esp_netif_sntp_init(&sntp);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
    }
    if (xTaskCreateWithCaps(alarm_task, "alarm", 3072, NULL, 3, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
