// SPDX-License-Identifier: Apache-2.0
#pragma once

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

#ifdef __cplusplus
}
#endif
