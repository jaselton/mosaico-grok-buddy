// SPDX-License-Identifier: Apache-2.0
#include "app_config.h"

#include <stdbool.h>
#include <string.h>

#include "mbedtls/platform_util.h"
#include "mosaico_shared.h"

static bool password_valid(const char *password)
{
    const size_t length = strlen(password);
    if (length == 0) {
        return true; /* A saved empty password represents an open network. */
    }
    if (length < 8 || length > 64) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)password[i];
        if (length == 64) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                    (c >= 'A' && c <= 'F'))) {
                return false;
            }
        } else if (c < 0x20 || c > 0x7e) {
            return false;
        }
    }
    return true;
}

esp_err_t app_config_load_wifi(app_config_wifi_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    mbedtls_platform_zeroize(out, sizeof(*out));
    esp_err_t err = mosaico_shared_get("wifi_ssid", out->ssid, sizeof(out->ssid));
    if (err == ESP_OK) {
        err = mosaico_shared_get("wifi_password", out->password, sizeof(out->password));
    }
    if (err == ESP_OK && (!out->ssid[0] || !password_valid(out->password))) {
        err = ESP_ERR_INVALID_ARG;
    }
    if (err != ESP_OK) {
        /* Never expose a partial pair or retain credentials from a prior read. */
        mbedtls_platform_zeroize(out, sizeof(*out));
    }
    return err;
}

esp_err_t app_config_load_secret(const char *key, char *out, size_t out_size)
{
    if (!key || !out || out_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    mbedtls_platform_zeroize(out, out_size);
    esp_err_t err = mosaico_shared_get(key, out, out_size);
    if (err == ESP_OK) {
        /* Trim whitespace that often sneaks in when pasting a key. */
        size_t len = strlen(out);
        while (len && (out[len - 1] == ' ' || out[len - 1] == '\n' || out[len - 1] == '\r' ||
                       out[len - 1] == '\t')) {
            out[--len] = '\0';
        }
        if (!len) {
            err = ESP_ERR_INVALID_ARG;
        }
    }
    if (err != ESP_OK) {
        mbedtls_platform_zeroize(out, out_size);
    }
    return err;
}
