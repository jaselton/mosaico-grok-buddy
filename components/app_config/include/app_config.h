// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ssid[33];      /* 1..32 bytes plus NUL. */
    char password[65];  /* Empty, 8..63 printable ASCII bytes, or 64 hex digits, plus NUL. */
} app_config_wifi_t;

/* Read saved Vibe Mode Wi-Fi credentials in task context; out must be non-NULL.
 * Returns ESP_OK only for a complete, valid pair; clears out on any failure.
 * The caller owns the snapshot and must clear it after use. */
esp_err_t app_config_load_wifi(app_config_wifi_t *out);

/* Read a custom shared string (e.g. an API key) saved in Vibe Mode, trimmed.
 * Returns ESP_ERR_NVS_NOT_FOUND when unset; clears out on any failure. */
esp_err_t app_config_load_secret(const char *key, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
