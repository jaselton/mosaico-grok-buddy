// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_ALARM_MAX       4
#define APP_ALARM_LABEL_MAX 40

/** Ring start/stop notifications; run on the alarm task, must not block. */
typedef void (*app_alarm_ring_cb_t)(bool ringing, const char *label, void *ctx);

/** Start the alarm service: SNTP clock (timezone from Kconfig), alarms persisted
 * in NVS, a 1 s checker. Call after nvs_flash_init(); Wi-Fi may come up later. */
esp_err_t app_alarm_start(app_alarm_ring_cb_t cb, void *ctx);

/** True once the wall clock has been set by SNTP. */
bool app_alarm_time_valid(void);

/** Format the current local time, e.g. "Wednesday, November 26, 7:05 AM". */
void app_alarm_format_now(char *out, size_t len);

/** Set a one-shot alarm at the next occurrence of hour:minute (local, 24 h).
 * Writes a spoken confirmation (e.g. "6:30 AM tomorrow") to out. */
esp_err_t app_alarm_set(int hour, int minute, const char *label, char *out, size_t len);

/** Set a one-shot alarm `seconds` from now (1 s .. 24 h); spoken confirmation in out. */
esp_err_t app_alarm_set_in(int seconds, const char *label, char *out, size_t len);

/** Daily "watch" alarm (the clock face's alarm hand): hour 0-23, minute 0-59.
 * Persisted; when on it rings every day at that local time. */
esp_err_t app_alarm_set_watch(int hour, int minute, bool on);
/** Read the watch alarm; returns false if it has never been set. */
bool app_alarm_get_watch(int *hour, int *minute, bool *on);

/** Cancel every pending alarm; returns how many were cancelled. */
int app_alarm_cancel_all(void);

/** Stop a ringing alarm (shake, pick-up, tap). Returns true if one was ringing. */
bool app_alarm_dismiss(void);

/** True while an alarm is ringing. */
bool app_alarm_ringing(void);

#ifdef __cplusplus
}
#endif
