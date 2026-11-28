// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_buddy.h"
#include "esp_gsp.h"

typedef struct {
    int volume;        /* 0-100 */
    int brightness;    /* 10-100, daytime */
    bool wake_word;
    bool night_dim;
} settings_prefs_t;

/** Loads saved settings, applies them and installs the pull-down gesture. UI task. */
void settings_start(esp_gsp_handle_t ui, const app_buddy_screen_ids_t *ids);
void settings_scene_changed(uint16_t scene_id);
/** Forward app_buddy call actions here (render task). */
void settings_on_call(uint16_t scene_id, uint16_t action, int32_t value);
/** If settings is showing, go back to where it was opened from. */
bool settings_close(void);
const settings_prefs_t *settings_prefs(void);
