// SPDX-License-Identifier: Apache-2.0
/* Wi-Fi station with a list of known networks:
 *   - networks joined from the on-device Wi-Fi page (NVS "wifinets", newest first, up to 5),
 *   - the Vibe Mode network,
 *   - an optional backup baked in at build time (e.g. a phone hotspot).
 * Boot tries the most recently used one. After two failures in a row it scans and
 * joins the strongest known network in range (cycling through the list when none is
 * visible). The Wi-Fi page can also scan on demand and join a new network, which is
 * then remembered. Passwords never reach the log. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "voice_internal.h"

static const char *TAG = "voice_wifi";

#define BIT_CONNECTED BIT0
#define RETRY_MIN_MS 1000
#define RETRY_MAX_MS 30000
#define FAILS_BEFORE_SWITCH 2
#define MAX_NETS 8
#define MAX_SAVED 5

typedef struct {
    char ssid[33];
    char pass[65];
    bool saved;   /* lives in NVS (joined on the device) */
} net_t;

static EventGroupHandle_t s_events;
static TimerHandle_t s_retry_timer;
static uint32_t s_retry_ms = RETRY_MIN_MS;
static net_t s_nets[MAX_NETS];
static int s_net_count, s_net, s_fails;
static bool s_switch, s_scan_for_switch, s_scan_for_ui;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static voice_wifi_scan_cb_t s_scan_cb;
static void *s_scan_ctx;
static char s_status[64] = "starting";
static int s_last_reason;

static void set_status(const char *fmt, const char *ssid)
{
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status, sizeof(s_status), fmt, ssid);
    portEXIT_CRITICAL(&s_lock);
}

static void fill_config(wifi_config_t *wc, const net_t *n)
{
    memset(wc, 0, sizeof(*wc));
    /* Fields need no NUL at full length (32-byte SSID, 64-hex PSK). */
    memcpy(wc->sta.ssid, n->ssid, strnlen(n->ssid, sizeof(wc->sta.ssid)));
    memcpy(wc->sta.password, n->pass, strnlen(n->pass, sizeof(wc->sta.password)));
    wc->sta.threshold.authmode = n->pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc->sta.pmf_cfg.capable = true;
}

/* Lower-case ASCII and fold curly apostrophes to ' so "Sam's Iphone" matches
 * the phone's broadcast "Sam’s iPhone". */
static void fold(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && in[i] && o + 1 < cap; ++i) {
        if (i + 2 < n && in[i] == 0xE2 && in[i + 1] == 0x80 && (in[i + 2] == 0x98 || in[i + 2] == 0x99)) {
            out[o++] = '\'';
            i += 2;
        } else {
            out[o++] = (char)tolower(in[i]);
        }
    }
    out[o] = '\0';
}

static void use_net(int i)
{
    wifi_config_t wc;
    s_net = i;
    fill_config(&wc, &s_nets[i]);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    set_status("Connecting to %s", s_nets[i].ssid);
}

/* ---- known networks in NVS ---- */

static void save_list(void)
{
    nvs_handle_t h;
    if (nvs_open("wifinets", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    int n = 0;
    char key[8];
    for (int i = 0; i < s_net_count && n < MAX_SAVED; ++i) {
        if (!s_nets[i].saved) {
            continue;
        }
        snprintf(key, sizeof(key), "s%d", n);
        nvs_set_str(h, key, s_nets[i].ssid);
        snprintf(key, sizeof(key), "p%d", n);
        nvs_set_str(h, key, s_nets[i].pass);
        ++n;
    }
    nvs_set_u8(h, "n", (uint8_t)n);
    nvs_commit(h);
    nvs_close(h);
}

static void load_saved(void)
{
    nvs_handle_t h;
    if (nvs_open("wifinets", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t n = 0;
    nvs_get_u8(h, "n", &n);
    char key[8];
    for (int i = 0; i < n && i < MAX_SAVED && s_net_count < MAX_NETS; ++i) {
        net_t *e = &s_nets[s_net_count];
        size_t len = sizeof(e->ssid);
        snprintf(key, sizeof(key), "s%d", i);
        if (nvs_get_str(h, key, e->ssid, &len) != ESP_OK || !e->ssid[0]) {
            continue;
        }
        len = sizeof(e->pass);
        snprintf(key, sizeof(key), "p%d", i);
        if (nvs_get_str(h, key, e->pass, &len) != ESP_OK) {
            e->pass[0] = '\0';
        }
        e->saved = true;
        ++s_net_count;
    }
    nvs_close(h);
}

static int find_net(const char *ssid)
{
    for (int i = 0; i < s_net_count; ++i) {
        if (!strcmp(s_nets[i].ssid, ssid)) {
            return i;
        }
    }
    return -1;
}

static void add_net(const char *ssid, const char *pass, bool saved)
{
    if (!ssid || !ssid[0] || find_net(ssid) >= 0 || s_net_count >= MAX_NETS) {
        return;
    }
    net_t *e = &s_nets[s_net_count++];
    snprintf(e->ssid, sizeof(e->ssid), "%s", ssid);
    snprintf(e->pass, sizeof(e->pass), "%s", pass ? pass : "");
    e->saved = saved;
}

/* ---- scanning ---- */

static void on_scan_done(void)
{
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 30) {
        n = 30;
    }
    wifi_ap_record_t *aps = n ? calloc(n, sizeof(*aps)) : NULL;
    if (!aps || esp_wifi_scan_get_ap_records(&n, aps) != ESP_OK) {
        esp_wifi_clear_ap_list();
        n = 0;
    }
    if (s_scan_for_ui) {
        s_scan_for_ui = false;
        voice_wifi_ap_t out[VOICE_WIFI_SCAN_MAX];
        int m = 0;
        for (int i = 0; i < n && m < VOICE_WIFI_SCAN_MAX; ++i) {
            if (!aps[i].ssid[0]) {
                continue;   /* hidden network */
            }
            bool dup = false;
            for (int j = 0; j < m && !dup; ++j) {
                dup = !strcmp(out[j].ssid, (const char *)aps[i].ssid);
            }
            if (dup) {
                continue;   /* mesh nodes: keep the strongest (scan results are sorted) */
            }
            snprintf(out[m].ssid, sizeof(out[m].ssid), "%s", (const char *)aps[i].ssid);
            out[m].rssi = aps[i].rssi;
            out[m].secure = aps[i].authmode != WIFI_AUTH_OPEN;
            out[m].known = find_net(out[m].ssid) >= 0;
            ++m;
        }
        if (s_scan_cb) {
            s_scan_cb(out, m, s_scan_ctx);
        }
    }
    if (s_scan_for_switch) {
        s_scan_for_switch = false;
        /* Strongest known network in range; a loose name match covers a hotspot typed
         * with a straight apostrophe. */
        int best = -1, best_rssi = -127;
        for (int k = 0; k < s_net_count; ++k) {
            char want[40], got[40];
            fold((const uint8_t *)s_nets[k].ssid, strlen(s_nets[k].ssid), want, sizeof(want));
            for (int i = 0; i < n; ++i) {
                fold(aps[i].ssid, sizeof(aps[i].ssid), got, sizeof(got));
                if (!strcmp(want, got) && aps[i].rssi > best_rssi) {
                    best = k;
                    best_rssi = aps[i].rssi;
                    if (strcmp(s_nets[k].ssid, (const char *)aps[i].ssid)) {
                        snprintf(s_nets[k].ssid, sizeof(s_nets[k].ssid), "%s", (const char *)aps[i].ssid);
                    }
                }
            }
        }
        if (best < 0) {
            best = (s_net + 1) % s_net_count;
            ESP_LOGW(TAG, "no known network in scan (%u seen); trying the next one", n);
        } else {
            ESP_LOGI(TAG, "known network in range (rssi %d)", best_rssi);
        }
        use_net(best);
        esp_wifi_connect();
    }
    free(aps);
}

static void retry_cb(TimerHandle_t t)
{
    (void)t;
    if (s_switch && s_net_count > 1) {
        s_switch = false;
        s_scan_for_switch = true;
        if (esp_wifi_scan_start(NULL, false) == ESP_OK) {
            return; /* connect from WIFI_EVENT_SCAN_DONE */
        }
        s_scan_for_switch = false;
        use_net((s_net + 1) % s_net_count);
    }
    esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        on_scan_done();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_last_reason = d ? d->reason : -1;
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        if (s_scan_for_switch) {
            return;   /* a switch is already under way */
        }
        const bool bad_pass = s_last_reason == WIFI_REASON_AUTH_FAIL || s_last_reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                              s_last_reason == WIFI_REASON_HANDSHAKE_TIMEOUT || s_last_reason == 15;
        set_status(bad_pass ? "%s: wrong password?" : "%s: not connected", s_nets[s_net].ssid);
        if (s_net_count > 1 && ++s_fails >= FAILS_BEFORE_SWITCH) {
            s_fails = 0;
            s_switch = true;
            s_retry_ms = RETRY_MIN_MS;
        }
        ESP_LOGW(TAG, "disconnected (reason %d); retry in %lu ms", s_last_reason, (unsigned long)s_retry_ms);
        xTimerChangePeriod(s_retry_timer, pdMS_TO_TICKS(s_retry_ms), 0);
        xTimerStart(s_retry_timer, 0);
        s_retry_ms = s_retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : s_retry_ms * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "connected, ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry_ms = RETRY_MIN_MS;
        s_fails = 0;
        set_status("%s", s_nets[s_net].ssid);
        /* Remember which one worked so the next boot starts there. */
        nvs_handle_t h;
        if (nvs_open("wifinets", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "last", s_nets[s_net].ssid);
            nvs_commit(h);
            nvs_close(h);
        }
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

esp_err_t voice_wifi_start(const char *ssid, const char *password, const char *backup_ssid,
                           const char *backup_password)
{
    s_events = xEventGroupCreate();
    s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(RETRY_MIN_MS), pdFALSE, NULL, retry_cb);
    if (!s_events || !s_retry_timer) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (!esp_netif_create_default_wifi_sta()) {
        return ESP_FAIL;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err == ESP_OK) {
        err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    }
    if (err != ESP_OK) {
        return err;
    }

    s_net_count = 0;
    load_saved();
    add_net(ssid, password, false);
    add_net(backup_ssid, backup_password, false);
    if (!s_net_count) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Start with the network that last worked. */
    int first = 0;
    nvs_handle_t h;
    if (nvs_open("wifinets", NVS_READONLY, &h) == ESP_OK) {
        char last[33] = "";
        size_t len = sizeof(last);
        if (nvs_get_str(h, "last", last, &len) == ESP_OK && find_net(last) >= 0) {
            first = find_net(last);
        }
        nvs_close(h);
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        use_net(first);
        /* Modem sleep adds latency to streamed audio; the device is USB powered. */
        esp_wifi_set_ps(WIFI_PS_NONE);
        err = esp_wifi_start();
    }
    ESP_LOGI(TAG, "station starting (%d known network%s)", s_net_count, s_net_count > 1 ? "s" : "");
    return err;
}

bool voice_wifi_wait_connected(uint32_t timeout_ms)
{
    if (!s_events) {
        return false;
    }
    return xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) &
           BIT_CONNECTED;
}

esp_err_t voice_wifi_scan(voice_wifi_scan_cb_t cb, void *ctx)
{
    if (!s_events) {
        return ESP_ERR_INVALID_STATE;
    }
    s_scan_cb = cb;
    s_scan_ctx = ctx;
    s_scan_for_ui = true;
    const esp_err_t err = esp_wifi_scan_start(NULL, false);
    if (err != ESP_OK) {
        s_scan_for_ui = false;
    }
    return err;
}

esp_err_t voice_wifi_join(const char *ssid, const char *password)
{
    if (!s_events || !ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    int i = find_net(ssid);
    if (i >= 0 && password) {
        snprintf(s_nets[i].pass, sizeof(s_nets[i].pass), "%s", password);
    }
    if (i < 0) {
        if (s_net_count >= MAX_NETS) {
            /* Full: drop the oldest saved network (the last saved entry in the list). */
            for (int k = s_net_count - 1; k >= 0; --k) {
                if (s_nets[k].saved) {
                    memmove(&s_nets[k], &s_nets[k + 1], (size_t)(s_net_count - k - 1) * sizeof(net_t));
                    --s_net_count;
                    break;
                }
            }
        }
        if (s_net_count >= MAX_NETS) {
            return ESP_ERR_NO_MEM;
        }
        i = s_net_count++;
        snprintf(s_nets[i].ssid, sizeof(s_nets[i].ssid), "%s", ssid);
        snprintf(s_nets[i].pass, sizeof(s_nets[i].pass), "%s", password ? password : "");
    }
    s_nets[i].saved = true;
    /* Newest first, so it's kept when the list is full. */
    if (i > 0) {
        const net_t tmp = s_nets[i];
        memmove(&s_nets[1], &s_nets[0], (size_t)i * sizeof(net_t));
        s_nets[0] = tmp;
    }
    save_list();
    ESP_LOGI(TAG, "joining a network from the Wi-Fi page");
    xTimerStop(s_retry_timer, 0);
    s_fails = 0;
    s_switch = false;
    s_retry_ms = RETRY_MIN_MS;
    esp_wifi_disconnect();
    use_net(0);
    return esp_wifi_connect();
}

esp_err_t voice_wifi_forget(const char *ssid)
{
    const int i = find_net(ssid);
    if (i < 0 || !s_nets[i].saved) {
        return ESP_ERR_NOT_FOUND;
    }
    memmove(&s_nets[i], &s_nets[i + 1], (size_t)(s_net_count - i - 1) * sizeof(net_t));
    --s_net_count;
    if (s_net >= s_net_count) {
        s_net = 0;
    }
    save_list();
    return ESP_OK;
}

void voice_wifi_status(char *out, size_t len)
{
    portENTER_CRITICAL(&s_lock);
    snprintf(out, len, "%s", s_status);
    portEXIT_CRITICAL(&s_lock);
}
