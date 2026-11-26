// SPDX-License-Identifier: Apache-2.0
/*
 * One-shot alarms for the desk buddy. The wall clock comes from SNTP (the
 * board has no battery-backed RTC), local time from CONFIG_GROK_BUDDY_TZ.
 * Alarms are stored as absolute epoch seconds in NVS so they survive a
 * reboot; one that came due while the board was off rings on boot only if
 * it is less than 10 minutes late. A ringing alarm stops on dismiss or after
 * RING_MAX_S, and reports start/stop through the callback.
 *
 * Flash writes need an internal-RAM stack, so only the alarm task (internal
 * stack) writes NVS; callers on other tasks just mark the list dirty.
 */
#include "app_alarm.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
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

typedef struct {
    int8_t hour, minute;   /* -1 = never set */
    uint8_t on;
    uint8_t once;          /* switch itself off after ringing (voice one-time alarms) */
    int64_t last_rang_day; /* local day number it last rang, to ring once per day */
    char label[APP_ALARM_LABEL_MAX];
} watch_t;

static struct {
    watch_t watch;
    alarm_t alarms[APP_ALARM_MAX];
    SemaphoreHandle_t lock;
    app_alarm_ring_cb_t cb;
    void *ctx;
    atomic_bool ringing;
    atomic_bool dismiss;
    atomic_bool ring_is_timer;
    atomic_bool dirty;      /* list changed; the alarm task persists it */
    char ring_label[APP_ALARM_LABEL_MAX];
    /* Countdown timer: monotonic, so it works before the clock syncs. Not
     * persisted (a reboot clears it). Guarded by lock. */
    struct {
        int total_s;
        int64_t end_us;        /* running: esp_timer deadline */
        int64_t remaining_us;  /* paused: what is left */
        bool running, done;
        char label[APP_ALARM_LABEL_MAX];
    } timer;
} a;

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, "list", a.alarms, sizeof(a.alarms));
    nvs_set_blob(h, "watch", &a.watch, sizeof(a.watch));
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
        len = sizeof(a.watch);
        if (nvs_get_blob(h, "watch", &a.watch, &len) != ESP_OK || len != sizeof(a.watch)) {
            a.watch.hour = a.watch.minute = -1;
            a.watch.on = 0;
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
        snprintf(out, len, "not synced yet (the device just started; it takes a few seconds)");
        return;
    }
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    strftime(out, len, "%A, %B %d, %I:%M:%S %p", &t);
}

static esp_err_t store(time_t when, const char *label);

static void describe(time_t when, char *out, size_t len)
{
    time_t now = time(NULL);
    struct tm tw, tn;
    localtime_r(&when, &tw);
    localtime_r(&now, &tn);
    char hm[16];
    strftime(hm, sizeof(hm), tw.tm_sec ? "%I:%M:%S %p" : "%I:%M %p", &tw);
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
    const esp_err_t err = store(when, label);
    if (err == ESP_OK) {
        describe(when, out, len);
        ESP_LOGI(TAG, "alarm set for %s", out);
    }
    return err;
}

esp_err_t app_alarm_set_in(int seconds, const char *label, char *out, size_t len)
{
    if (seconds < 1 || seconds > 24 * 3600) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!app_alarm_time_valid()) {
        return ESP_ERR_INVALID_STATE;
    }
    const time_t when = time(NULL) + seconds;
    const esp_err_t err = store(when, label);
    if (err == ESP_OK) {
        describe(when, out, len);
        ESP_LOGI(TAG, "alarm set for %s (in %d s)", out, seconds);
    }
    return err;
}

static esp_err_t store(time_t when, const char *label)
{
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
    atomic_store(&a.dirty, true);
    xSemaphoreGive(a.lock);
    return ESP_OK;
}

esp_err_t app_alarm_set_watch(int hour, int minute, bool on)
{
    return app_alarm_set_watch_ex(hour, minute, on, false, NULL);
}

esp_err_t app_alarm_set_watch_ex(int hour, int minute, bool on, bool once, const char *label)
{
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(a.lock, portMAX_DELAY);
    const bool moved = a.watch.hour != hour || a.watch.minute != minute;
    a.watch.hour = (int8_t)hour;
    a.watch.minute = (int8_t)minute;
    a.watch.on = on;
    a.watch.once = once;
    snprintf(a.watch.label, sizeof(a.watch.label), "%s", label ? label : "");
    if (moved || on) {
        a.watch.last_rang_day = 0;   /* a new time may ring today */
    }
    atomic_store(&a.dirty, true);
    xSemaphoreGive(a.lock);
    ESP_LOGI(TAG, "watch alarm %02d:%02d %s%s", hour, minute, on ? "ON" : "off", once ? " (once)" : "");
    return ESP_OK;
}

bool app_alarm_get_watch(int *hour, int *minute, bool *on)
{
    if (a.watch.hour < 0) {
        return false;
    }
    if (hour) {
        *hour = a.watch.hour;
    }
    if (minute) {
        *minute = a.watch.minute;
    }
    if (on) {
        *on = a.watch.on;
    }
    return true;
}

int app_alarm_cancel_all(void)
{
    xSemaphoreTake(a.lock, portMAX_DELAY);
    int n = 0;
    for (int i = 0; i < APP_ALARM_MAX; ++i) {
        n += a.alarms[i].when != 0;
    }
    memset(a.alarms, 0, sizeof(a.alarms));
    if (a.watch.on) {
        a.watch.on = 0;   /* the hand stays where it is, switched off */
        n++;
    }
    if (a.timer.total_s && !a.timer.done) {
        n++;
    }
    memset(&a.timer, 0, sizeof(a.timer));
    atomic_store(&a.dirty, true);
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

esp_err_t app_alarm_timer_set(int seconds, const char *label, bool start)
{
    if (seconds < 1 || seconds > APP_ALARM_TIMER_MAX_S || !a.lock) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(a.lock, portMAX_DELAY);
    a.timer.total_s = seconds;
    a.timer.remaining_us = (int64_t)seconds * 1000000;
    a.timer.done = false;
    a.timer.running = start;
    a.timer.end_us = esp_timer_get_time() + a.timer.remaining_us;
    snprintf(a.timer.label, sizeof(a.timer.label), "%s", label ? label : "");
    xSemaphoreGive(a.lock);
    ESP_LOGI(TAG, "timer %d s%s%s", seconds, start ? " started" : " set", label && label[0] ? "" : "");
    return ESP_OK;
}

void app_alarm_timer_toggle(void)
{
    if (!a.lock) {
        return;
    }
    xSemaphoreTake(a.lock, portMAX_DELAY);
    const int64_t now = esp_timer_get_time();
    if (a.timer.running) {
        a.timer.remaining_us = a.timer.end_us - now;
        a.timer.running = false;
    } else if (a.timer.total_s > 0 && !a.timer.done && a.timer.remaining_us > 0) {
        a.timer.end_us = now + a.timer.remaining_us;
        a.timer.running = true;
    }
    xSemaphoreGive(a.lock);
}

void app_alarm_timer_reset(void)
{
    if (!a.lock) {
        return;
    }
    xSemaphoreTake(a.lock, portMAX_DELAY);
    memset(&a.timer, 0, sizeof(a.timer));
    xSemaphoreGive(a.lock);
}

bool app_alarm_timer_get(app_alarm_timer_t *out)
{
    if (!a.lock) {
        return false;
    }
    xSemaphoreTake(a.lock, portMAX_DELAY);
    out->total_s = a.timer.total_s;
    out->running = a.timer.running;
    out->done = a.timer.done;
    int64_t rem = a.timer.running ? a.timer.end_us - esp_timer_get_time() : a.timer.remaining_us;
    out->remaining_ms = rem < 0 ? 0 : (int32_t)(rem / 1000);
    snprintf(out->label, sizeof(out->label), "%s", a.timer.label);
    xSemaphoreGive(a.lock);
    return out->total_s > 0;
}

bool app_alarm_ring_is_timer(void)
{
    return atomic_load(&a.ring_is_timer);
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
        vTaskDelay(pdMS_TO_TICKS(250));
        if (atomic_exchange(&a.dirty, false)) {
            xSemaphoreTake(a.lock, portMAX_DELAY);
            save();
            xSemaphoreGive(a.lock);
        }
        const int64_t mono_s = esp_timer_get_time() / 1000000;
        if (atomic_load(&a.ringing)) {
            if (atomic_exchange(&a.dismiss, false) || mono_s - ring_started > RING_MAX_S) {
                atomic_store(&a.ringing, false);
                ESP_LOGI(TAG, "alarm stopped");
                if (a.cb) {
                    a.cb(false, a.ring_label, a.ctx);
                }
            }
            continue;
        }
        /* Countdown timer (works without a synced clock) */
        bool timer_due = false;
        xSemaphoreTake(a.lock, portMAX_DELAY);
        if (a.timer.running && esp_timer_get_time() >= a.timer.end_us) {
            a.timer.running = false;
            a.timer.done = true;
            a.timer.remaining_us = 0;
            snprintf(a.ring_label, sizeof(a.ring_label), "%s", a.timer.label[0] ? a.timer.label : "timer");
            timer_due = true;
        }
        xSemaphoreGive(a.lock);
        if (timer_due) {
            atomic_store(&a.dismiss, false);
            ring_started = mono_s;
            atomic_store(&a.ring_is_timer, true);
            atomic_store(&a.ringing, true);
            ESP_LOGI(TAG, "timer done: %s", a.ring_label);
            if (a.cb) {
                a.cb(true, a.ring_label, a.ctx);
            }
            continue;
        }
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
        } else if (a.watch.on && a.watch.hour >= 0) {
            /* Daily watch alarm: ring within the minute it's set for, once per day. */
            time_t nt = (time_t)now;
            struct tm lt;
            localtime_r(&nt, &lt);
            const int64_t day = (int64_t)(lt.tm_year * 400 + lt.tm_yday);
            if (lt.tm_hour == a.watch.hour && lt.tm_min == a.watch.minute && a.watch.last_rang_day != day) {
                a.watch.last_rang_day = day;
                snprintf(a.ring_label, sizeof(a.ring_label), "%s", a.watch.label[0] ? a.watch.label : "wake-up alarm");
                if (a.watch.once) {
                    a.watch.on = 0;
                }
                due = APP_ALARM_MAX;   /* any non-negative value: ring */
                changed = true;
            }
        }
        if (changed) {
            save();
        }
        xSemaphoreGive(a.lock);
        booted = false;
        if (due >= 0) {
            ring_started = mono_s;
            atomic_store(&a.ring_is_timer, false);
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
    if (xTaskCreate(alarm_task, "alarm", 3584, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
