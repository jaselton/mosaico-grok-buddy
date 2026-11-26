// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>

#include "app_buddy.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Shared Vibe Mode keys this component reads (Settings > Shared information). */
#define APP_VOICE_KEY_API_KEY "xai_api_key"   /**< Required xAI API key. */
#define APP_VOICE_KEY_VOICE   "xai_voice"     /**< Optional voice id, default "eve". */

typedef struct {
    app_buddy_handle_t buddy;   /**< Face to drive; must outlive the voice service. */
} app_voice_config_t;

/** Start Wi-Fi, audio and the tap-to-talk Grok voice service; routes buddy taps.
 * Returns ESP_ERR_NOT_FOUND when Wi-Fi or the API key is not provisioned. */
esp_err_t app_voice_start(const app_voice_config_t *config);

/** Mute (asleep) or unmute the voice from any task: closes any session, ignores
 * taps and holds Grok Bot announcements until awake. */
void app_voice_set_asleep(bool asleep);

/** Alarm ringing (repeating chime, conversation stopped) or stopped (Grok greets). */
void app_voice_set_alarm(bool ringing, const char *label);

/** Usage summary from the relay (GET /summary). The handler runs on the relay
 * task with the JSON body, or NULL if the fetch failed. */
typedef void (*app_voice_summary_cb_t)(const char *json, void *ctx);
void app_voice_set_summary_handler(app_voice_summary_cb_t cb, void *ctx);
/** Ask for a fresh summary soon (no-op without the relay). */
void app_voice_request_summary(void);

/** Workout and timer beeps through the speaker (any task, non-blocking). */
typedef enum {
    APP_VOICE_BEEP_COUNT,   /* 3-2-1 tick */
    APP_VOICE_BEEP_START,   /* get-ready began */
    APP_VOICE_BEEP_WORK,    /* go */
    APP_VOICE_BEEP_REST,
    APP_VOICE_BEEP_DONE,
} app_voice_beep_t;
void app_voice_beep(app_voice_beep_t pattern);

/** Night mode (from any task): wake word off and announcements held until
 * morning; a tap still talks and lifts it for a while. */
void app_voice_set_night(bool night);
/** True while night mode is in effect (not lifted by a tap or an alarm). */
bool app_voice_night_active(void);

/** Grok asked to show a screen (a timer or workout it just started). */
typedef enum {
    APP_VOICE_SCREEN_TIMER,
    APP_VOICE_SCREEN_WORKOUT,
} app_voice_screen_t;
typedef void (*app_voice_screen_cb_t)(app_voice_screen_t screen, void *ctx);
void app_voice_set_screen_handler(app_voice_screen_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
