// SPDX-License-Identifier: Apache-2.0
/*
 * Pull-down settings: drag down from the top edge of any screen. Volume,
 * screen brightness, the "Hey Grok" wake word and night dimming, plus the
 * Wi-Fi network. Values live in NVS ("prefs") and apply at boot. Done, a
 * swipe up from the bottom edge, or the AI button returns to the screen you
 * came from.
 */
#include "settings.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_clock.h"
#include "app_voice.h"
#include "bsp/display.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

static const char *TAG = "settings";

#define EDGE_PX 44         /* touches starting this close to the top (or bottom) edge */
#define PULL_PX 70         /* how far to pull before it opens */
#define SAVE_DELAY_US (1500LL * 1000)

static struct {
    esp_gsp_handle_t ui;
    app_buddy_screen_ids_t id;
    settings_prefs_t p;
    uint16_t current, back_to;
    /* gesture (render task) */
    bool down, tracking, fired, from_bottom;
    int32_t x0, y0;
    /* render task -> UI tick */
    atomic_int want_open, want_close, vol, br, wake, dim;
    int64_t save_at_us, beep_at_us;   /* one preview beep after the volume slider settles */
    bool br_pending;                  /* apply saved brightness on the UI task */
    bool opening;                     /* settings reached by the pull-down, not a sideways swipe */
    /* Wi-Fi page */
    app_voice_wifi_ap_t aps[APP_VOICE_WIFI_SCAN_MAX], scan_in[APP_VOICE_WIFI_SCAN_MAX];
    int n_aps, n_in, sel;
    bool scanning;
    portMUX_TYPE scan_lock;
    atomic_int scan_new, want_wifi, want_back, want_scan, row, want_cancel, kb_ok, entered;
} st = { .scan_lock = portMUX_INITIALIZER_UNLOCKED, .p = { .volume = 82, .brightness = 100, .wake_word = true, .night_dim = true } };

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open("prefs", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t v;
    if (nvs_get_u8(h, "vol", &v) == ESP_OK && v <= 100) st.p.volume = v;
    if (nvs_get_u8(h, "bright", &v) == ESP_OK && v >= 10 && v <= 100) st.p.brightness = v;
    if (nvs_get_u8(h, "wake", &v) == ESP_OK) st.p.wake_word = v != 0;
    if (nvs_get_u8(h, "dim", &v) == ESP_OK) st.p.night_dim = v != 0;
    nvs_close(h);
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open("prefs", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, "vol", (uint8_t)st.p.volume);
    nvs_set_u8(h, "bright", (uint8_t)st.p.brightness);
    nvs_set_u8(h, "wake", st.p.wake_word);
    nvs_set_u8(h, "dim", st.p.night_dim);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved: vol %d bright %d wake %d dim %d", st.p.volume, st.p.brightness, st.p.wake_word,
             st.p.night_dim);
}

const settings_prefs_t *settings_prefs(void)
{
    return &st.p;
}

/* ---- pull-down gesture (render task) -------------------------------------- */

static bool intercept(esp_gsp_handle_t gsp, int32_t x, int32_t y, bool pressed, void *ctx)
{
    (void)gsp;
    (void)ctx;
    const bool on_settings = st.current == st.id.settings_scene;
    if (pressed && !st.down) {                       /* touch began */
        st.down = true;
        st.fired = false;
        st.from_bottom = on_settings && y > 480 - 26;   /* below the Done button */
        st.tracking = (!on_settings && y < EDGE_PX) || st.from_bottom;
        st.x0 = x;
        st.y0 = y;
    } else if (!pressed) {
        st.down = false;
    }
    if (!st.tracking) {
        return app_clock_input(x, y, pressed);       /* the one interceptor slot: chain to the clock */
    }
    /* Movement samples may not reach the interceptor, so the release decides too. */
    if (!st.fired) {
        const int32_t dy = st.from_bottom ? st.y0 - y : y - st.y0, dx = abs(x - st.x0);
        if (dy > PULL_PX && dx < dy) {
            st.fired = true;
            atomic_store(st.from_bottom ? &st.want_close : &st.want_open, 1);
        }
    }
    if (!pressed) {
        st.tracking = false;
    }
    return true;                                     /* the edge band belongs to the gesture */
}

/* ---- controls (render task: just record) ----------------------------------- */

static void wifi_on_call(uint16_t scene_id, uint16_t action, int32_t value);

void settings_on_call(uint16_t scene_id, uint16_t action, int32_t value)
{
    if (scene_id == st.id.wifi_scene || scene_id == st.id.pass_scene) {
        wifi_on_call(scene_id, action, value);
        return;
    }
    if (scene_id != st.id.settings_scene) {
        return;
    }
    if (action == st.id.settings_act_vol) {
        atomic_store(&st.vol, value + 1);            /* 0 = nothing pending */
    } else if (action == st.id.settings_act_br) {
        atomic_store(&st.br, value + 1);
    } else if (action == st.id.settings_act_wake) {
        atomic_store(&st.wake, 1);
    } else if (action == st.id.settings_act_dim) {
        atomic_store(&st.dim, 1);
    } else if (action == st.id.settings_act_done) {
        atomic_store(&st.want_close, 1);
    } else if (action == st.id.settings_act_wifi) {
        atomic_store(&st.want_wifi, 1);
    }
}

static void wifi_on_call(uint16_t scene_id, uint16_t action, int32_t value)
{
    if (scene_id == st.id.wifi_scene) {
        if (action == st.id.wifi_act_back) atomic_store(&st.want_back, 1);
        else if (action == st.id.wifi_act_scan) atomic_store(&st.want_scan, 1);
        else if (action == st.id.wifi_act_row) atomic_store(&st.row, value + 1);
    } else if (scene_id == st.id.pass_scene) {
        if (action == st.id.pass_act_cancel) atomic_store(&st.want_cancel, 1);
        else if (action == st.id.pass_act_key) atomic_store(&st.kb_ok, 1);   /* only OK reaches the app */
    }
}

/* Wi-Fi event task. */
static void on_scan(const app_voice_wifi_ap_t *aps, int count, void *ctx)
{
    (void)ctx;
    portENTER_CRITICAL(&st.scan_lock);
    st.n_in = count < APP_VOICE_WIFI_SCAN_MAX ? count : APP_VOICE_WIFI_SCAN_MAX;
    memcpy(st.scan_in, aps, (size_t)st.n_in * sizeof(*aps));
    portEXIT_CRITICAL(&st.scan_lock);
    atomic_store(&st.scan_new, 1);
}

/* ---- UI task ---------------------------------------------------------------- */

/* Network names are UTF-8 (an iPhone's is "Name’s iPhone"); the label fonts are
 * ASCII, so turn smart quotes into plain ones and drop anything else outside it. */
static void shown(const char *in, char *out, size_t len)
{
    size_t j = 0;
    const unsigned char *s = (const unsigned char *)in;
    while (*s && j + 1 < len) {
        if (s[0] < 0x80) {
            out[j++] = (char)(s[0] >= ' ' ? s[0] : ' ');
            s++;
        } else if (s[0] == 0xE2 && s[1] == 0x80 && s[2] >= 0x98 && s[2] <= 0x9F) {
            out[j++] = s[2] <= 0x9B ? '\'' : '"';
            s += 3;
        } else {
            out[j++] = ' ';
            s++;
            while ((*s & 0xC0) == 0x80) {
                s++;
            }
        }
    }
    out[j] = '\0';
}

/* The network we're on right now, raw bytes, or "" when not connected. */
static void current_ssid(char out[33])
{
    wifi_ap_record_t ap;
    out[0] = '\0';
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        memcpy(out, ap.ssid, 32);
        out[32] = '\0';
    }
}

static void show_values(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%d%%", st.p.volume);
    esp_gsp_set_text(st.ui, st.id.settings_volv, buf);
    snprintf(buf, sizeof(buf), "%d%%", st.p.brightness);
    esp_gsp_set_text(st.ui, st.id.settings_brv, buf);
}

static void show_wifi(void)
{
    char buf[80] = "Wi-Fi not connected";
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        const char *q = ap.rssi >= -55 ? "strong" : ap.rssi >= -67 ? "good" : ap.rssi >= -75 ? "fair" : "weak";
        esp_netif_ip_info_t ip = { 0 };
        esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (n) {
            esp_netif_get_ip_info(n, &ip);
        }
        char raw[33], ssid[40];
        memcpy(raw, ap.ssid, 32);
        raw[32] = '\0';
        shown(raw, ssid, sizeof(ssid));
        snprintf(buf, sizeof(buf), "Wi-Fi %s · %s · " IPSTR, ssid, q, IP2STR(&ip.ip));
    }
    esp_gsp_set_text(st.ui, st.id.settings_wifi, buf);
}

/* ---- Wi-Fi page (UI task) ---- */

/* Only touch a label when its text changes: rewriting a dozen labels every second
 * keeps the renderer busy for nothing. */
static char s_shown_text[16][44];
static void set_if_changed(int slot, uint16_t bind, const char *text)
{
    if (strncmp(s_shown_text[slot], text, sizeof(s_shown_text[0]) - 1) == 0 &&
        strlen(text) < sizeof(s_shown_text[0])) {
        return;
    }
    snprintf(s_shown_text[slot], sizeof(s_shown_text[0]), "%s", text);
    esp_gsp_set_text(st.ui, bind, text);
}

static void wifi_rows(void)
{
    char buf[40], name[40], cur[33];
    current_ssid(cur);
    for (int i = 0; i < 6; ++i) {
        if (i < st.n_aps) {
            const app_voice_wifi_ap_t *a = &st.aps[i];
            shown(a->ssid, name, sizeof(name));
            set_if_changed(i, st.id.wifi_name[i], name);
            const char *q = a->rssi >= -55 ? "strong" : a->rssi >= -67 ? "good" : a->rssi >= -75 ? "fair" : "weak";
            if (cur[0] && strcmp(cur, a->ssid) == 0) {
                snprintf(buf, sizeof(buf), "connected");
            } else {
                snprintf(buf, sizeof(buf), "%s%s", a->known ? "saved \u00b7 " : (a->secure ? "" : "open \u00b7 "), q);
            }
            set_if_changed(6 + i, st.id.wifi_sig[i], buf);
        } else {
            set_if_changed(i, st.id.wifi_name[i], i == 0 && st.scanning ? "Looking for networks..." : "");
            set_if_changed(6 + i, st.id.wifi_sig[i], "");
        }
    }
}

static void wifi_status(void)
{
    char s[64], name[64], cur[33], buf[80];
    app_voice_wifi_status(s, sizeof(s));
    shown(s, name, sizeof(name));
    current_ssid(cur);
    /* A bare name means we're on it; anything else already says what's happening. */
    snprintf(buf, sizeof(buf), "%s%s", cur[0] && strcmp(cur, s) == 0 ? "Connected to " : "", name);
    set_if_changed(12, st.id.wifi_stat, buf);
}

static void wifi_scan(void)
{
    st.scanning = app_voice_wifi_scan(on_scan, NULL) == ESP_OK;
    if (!st.n_aps) {
        wifi_rows();
    }
}

static void wifi_join(const char *ssid, const char *pass)
{
    app_voice_wifi_join(ssid, pass);
    esp_gsp_goto_scene(st.ui, st.id.wifi_scene, ESP_GSP_CROSS_FADE);
}

static void wifi_tick(esp_gsp_handle_t ui)
{
    const bool on_list = st.current == st.id.wifi_scene, on_pass = st.current == st.id.pass_scene;
    const int entered = atomic_exchange(&st.entered, 0);
    if (atomic_exchange(&st.want_wifi, 0) && st.current == st.id.settings_scene) {
        esp_gsp_goto_scene(ui, st.id.wifi_scene, ESP_GSP_CROSS_FADE);
    }
    if (on_list && entered) {
        for (int i = 0; i < 16; ++i) {
            strcpy(s_shown_text[i], "\x01");     /* scene was rebuilt: repaint everything */
        }
        wifi_rows();
        wifi_status();
        wifi_scan();
    }
    if (on_pass && entered) {
        char buf[64], name[40] = "";
        if (st.sel < st.n_aps) {
            shown(st.aps[st.sel].ssid, name, sizeof(name));
        }
        snprintf(buf, sizeof(buf), "Password for %s", name);
        esp_gsp_set_text(ui, st.id.pass_name, buf);
        esp_gsp_keyboard_attach_ex(ui, st.id.pass_act_key, st.id.pass_text, 64);
    }
    if (atomic_exchange(&st.scan_new, 0)) {
        portENTER_CRITICAL(&st.scan_lock);
        st.n_aps = st.n_in;
        memcpy(st.aps, st.scan_in, (size_t)st.n_aps * sizeof(st.aps[0]));
        portEXIT_CRITICAL(&st.scan_lock);
        st.scanning = false;
        if (on_list) {
            wifi_rows();
        }
    }
    if (atomic_exchange(&st.want_scan, 0) && on_list) {
        wifi_scan();
    }
    int r = atomic_exchange(&st.row, 0);
    if (r && on_list && r - 1 < st.n_aps) {
        st.sel = r - 1;
        const app_voice_wifi_ap_t *a = &st.aps[st.sel];
        ESP_LOGI(TAG, "wifi row %d tapped (%s)", st.sel, a->known ? "saved" : a->secure ? "needs password" : "open");
        if (a->known) {
            wifi_join(a->ssid, NULL);            /* keep the saved password */
            wifi_status();
        } else if (!a->secure) {
            wifi_join(a->ssid, "");
            wifi_status();
        } else {
            esp_gsp_goto_scene(ui, st.id.pass_scene, ESP_GSP_CROSS_FADE);
        }
    }
    if (atomic_exchange(&st.kb_ok, 0) && on_pass) {
        char pw[72] = "";
        esp_gsp_keyboard_text(ui, pw, sizeof(pw));
        esp_gsp_keyboard_attach(ui, ESP_GSP_KEYBOARD_NONE, 0);
        if (st.sel < st.n_aps) {
            wifi_join(st.aps[st.sel].ssid, pw);
        }
        memset(pw, 0, sizeof(pw));
    }
    if (atomic_exchange(&st.want_cancel, 0) && on_pass) {
        esp_gsp_keyboard_attach(ui, ESP_GSP_KEYBOARD_NONE, 0);
        esp_gsp_goto_scene(ui, st.id.wifi_scene, ESP_GSP_CROSS_FADE);
    }
    if (atomic_exchange(&st.want_back, 0) && on_list) {
        st.opening = true;                       /* a real return, not a stray swipe */
        esp_gsp_goto_scene(ui, st.id.settings_scene, ESP_GSP_CROSS_FADE);
    }
    static int64_t last;
    if (on_list && esp_timer_get_time() - last > 1000LL * 1000) {
        last = esp_timer_get_time();
        wifi_status();
        wifi_rows();                             /* move the "connected" tag when it changes */
    }
}

static void apply_brightness(void)
{
    if (!app_voice_night_active()) {
        bsp_display_brightness_set(st.p.brightness);
    }
}

static void tick(esp_gsp_handle_t ui, void *ctx)
{
    (void)ctx;
    if (atomic_exchange(&st.want_open, 0) && st.current != st.id.settings_scene) {
        st.back_to = st.current;
        st.opening = true;
        esp_gsp_goto_scene(ui, st.id.settings_scene, ESP_GSP_CROSS_FADE);
    }
    if (atomic_exchange(&st.want_close, 0) && st.current == st.id.settings_scene) {
        esp_gsp_goto_scene(ui, st.back_to, ESP_GSP_CROSS_FADE);
    }
    int v;
    if ((v = atomic_exchange(&st.vol, 0))) {
        st.p.volume = v - 1;
        app_voice_set_volume(st.p.volume);
        show_values();
        st.save_at_us = esp_timer_get_time() + SAVE_DELAY_US;
        st.beep_at_us = esp_timer_get_time() + 400LL * 1000;
    }
    if (st.beep_at_us && esp_timer_get_time() >= st.beep_at_us) {
        st.beep_at_us = 0;
        app_voice_beep(APP_VOICE_BEEP_COUNT);        /* one short tick at the new level */
    }
    if (st.br_pending) {
        st.br_pending = false;
        apply_brightness();
    }
    if ((v = atomic_exchange(&st.br, 0))) {
        st.p.brightness = v - 1 < 10 ? 10 : v - 1;
        apply_brightness();
        show_values();
        st.save_at_us = esp_timer_get_time() + SAVE_DELAY_US;
    }
    if (atomic_exchange(&st.wake, 0)) {
        bool on = st.p.wake_word;
        esp_gsp_component_get_checked(ui, st.id.settings_key_wake, &on);
        st.p.wake_word = on;
        app_voice_set_wake_word(on);
        st.save_at_us = esp_timer_get_time() + SAVE_DELAY_US;
    }
    if (atomic_exchange(&st.dim, 0)) {
        bool on = st.p.night_dim;
        esp_gsp_component_get_checked(ui, st.id.settings_key_dim, &on);
        st.p.night_dim = on;
        st.save_at_us = esp_timer_get_time() + SAVE_DELAY_US;
    }
    if (st.save_at_us && esp_timer_get_time() >= st.save_at_us) {
        st.save_at_us = 0;
        save();
    }
    wifi_tick(ui);
    static int64_t last_wifi;
    if (st.current == st.id.settings_scene && esp_timer_get_time() - last_wifi > 5LL * 1000 * 1000) {
        last_wifi = esp_timer_get_time();
        show_wifi();
    }
}

void settings_scene_changed(uint16_t scene_id)
{
    const uint16_t prev = st.current;
    st.current = scene_id;
    if (scene_id == st.id.wifi_scene || scene_id == st.id.pass_scene) {
        atomic_store(&st.entered, 1);
        return;
    }
    if (scene_id != st.id.settings_scene) {
        return;
    }
    if (prev == st.id.wifi_scene) {
        st.opening = true;                       /* back from the Wi-Fi page */
    }
    if (!st.opening) {
        /* Settings is in the bundle, so a sideways swipe off the last screen can land
         * here: send it straight back. It only opens from the pull-down. */
        st.back_to = prev;
        atomic_store(&st.want_close, 1);
        return;
    }
    st.opening = false;
    /* The scene is rebuilt on arrival: put the saved values back on the controls. */
    esp_gsp_component_set_value(st.ui, st.id.settings_key_vol, st.p.volume);
    esp_gsp_component_set_value(st.ui, st.id.settings_key_br, st.p.brightness);
    esp_gsp_component_set_checked(st.ui, st.id.settings_key_wake, st.p.wake_word);
    esp_gsp_component_set_checked(st.ui, st.id.settings_key_dim, st.p.night_dim);
    show_values();
    show_wifi();
}

bool settings_close(void)
{
    if (st.current != st.id.settings_scene) {
        return false;
    }
    atomic_store(&st.want_close, 1);
    return true;
}

void settings_start(esp_gsp_handle_t ui, const app_buddy_screen_ids_t *ids)
{
    st.ui = ui;
    st.id = *ids;
    load();
    app_voice_set_volume(st.p.volume);
    app_voice_set_wake_word(st.p.wake_word);
    /* Not here: this runs on the main task, and a panel command sent while the UI
     * task is pushing a frame collides on the SPI bus (assert in
     * spi_device_release_bus, or a hung display). The first tick applies it. */
    st.br_pending = true;
    esp_gsp_set_input_interceptor(ui, intercept, NULL);
    if (!esp_gsp_timer_create(ui, 50, tick, NULL)) {
        ESP_LOGE(TAG, "no UI timer left (raise CONFIG_ESP_GSP_MAX_TIMERS)");
    }
    ESP_LOGI(TAG, "pull down from the top for settings");
}
