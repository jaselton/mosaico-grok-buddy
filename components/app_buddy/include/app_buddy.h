// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_gsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque Grok Bot eyes controller; ownership and threading: ../README.md. */
typedef struct app_buddy *app_buddy_handle_t;

/** Face states. Without a tap handler, a tap cycles through them in order. */
typedef enum {
    APP_BUDDY_IDLE = 0,
    APP_BUDDY_LISTENING = 1,    /**< Eyes on the user; bars follow the mic level. */
    APP_BUDDY_THINKING = 2,
    APP_BUDDY_SPEAKING = 3,     /**< Bars follow the playback level when fed. */
    APP_BUDDY_STATE_COUNT
} app_buddy_state_t;

/** Tap callback; runs on the GSP render task, so it must not block. */
typedef void (*app_buddy_tap_cb_t)(void *ctx);

/** Value copy for diagnostics; no pointers into private controller state. */
typedef struct {
    app_buddy_state_t state;    /**< Current face state. */
    uint32_t frames;            /**< Animation ticks since create. */
    uint32_t blinks;            /**< Blinks started since create. */
    uint32_t taps;              /**< Taps received since create. */
    esp_gsp_err_t last_error;   /**< Most recent setter error, or ESP_GSP_OK. */
} app_buddy_snapshot_t;

/** Create the eyes controller for ui and start its 30 fps animation timer.
 * ui must outlive the controller. Returns ESP_GSP_OK or a GSP error. */
esp_gsp_err_t app_buddy_create(esp_gsp_handle_t ui, app_buddy_handle_t *out_handle);

/** Request a face state (for future live Grok Bot status hooks).
 * Call from the UI loop; returns ESP_GSP_ERR_INVALID_ARG for a bad state. */
esp_gsp_err_t app_buddy_set_state(app_buddy_handle_t handle, app_buddy_state_t state);

/** Request a face state from any task; applied on the next animation tick.
 * Returns ESP_GSP_ERR_INVALID_ARG for a bad handle or state. */
esp_gsp_err_t app_buddy_request_state(app_buddy_handle_t handle, app_buddy_state_t state);

/** Feed a live audio level (0..255) from any task to drive the bars.
 * Until first called, the bars use a synthetic speech envelope. */
void app_buddy_set_level(app_buddy_handle_t handle, uint8_t level);

/** Route taps to cb instead of cycling states; NULL restores cycling.
 * Call from the UI loop or once right after create. */
void app_buddy_set_tap_handler(app_buddy_handle_t handle, app_buddy_tap_cb_t cb, void *ctx);

/** Copy diagnostics; call on the UI loop or after dispatch stops. */
esp_gsp_err_t app_buddy_read(app_buddy_handle_t handle, app_buddy_snapshot_t *out_snapshot);

/** Detach callbacks, stop the timer and free the controller.
 * Quiesce dispatch first; retains the handle if cleanup fails. */
esp_gsp_err_t app_buddy_destroy(app_buddy_handle_t *handle);

#ifdef __cplusplus
}
#endif
