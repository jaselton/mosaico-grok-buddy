// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include "app_buddy.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called on the motion task when the buddy is laid face-down (asleep) or picked back up. */
typedef void (*app_motion_sleep_cb_t)(bool asleep, void *ctx);

typedef struct {
    app_buddy_handle_t buddy;         /**< Face to drive; must outlive the service. */
    app_motion_sleep_cb_t on_sleep;   /**< Optional; e.g. mute the voice while asleep. */
    void (*on_shake)(void *ctx);      /**< Optional; e.g. dismiss a ringing alarm. */
    void *ctx;
} app_motion_config_t;

/** Start the IMU + haptics service: shake -> dizzy, bump -> startle, tilt -> look
 * downhill, face-down -> sleep. Calibrates "upright" from the first second. */
esp_err_t app_motion_start(const app_motion_config_t *config);

/** Repeating alarm buzz (on) until cleared (off). */
void app_motion_set_alarm(bool ringing);

/** Short purr on the vibration motor (non-blocking; safe from the render task). */
void app_motion_purr(void);

#ifdef __cplusplus
}
#endif
