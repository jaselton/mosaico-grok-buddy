// SPDX-License-Identifier: Apache-2.0
/*
 * Usage screen: weekly included Grok usage % (big number, main blue ring),
 * extra spend against the monthly cap (thin inner ring), today's voice minutes.
 *
 * Data comes from the relay's GET /summary (fetched on the voice relay task,
 * so no second TLS session competes for internal RAM). Team numbers are pushed
 * to the relay by a helper on the owner's Mac; if they are missing or stale the
 * screen says so. Refreshes on arrival and once a minute while visible.
 */
#include "clock_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_voice.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "usage_face";

#define FW 480
#define CX 240.0f
#define CY 240.0f
#define R_WEEK 204.0f
#define R_MONTH 180.0f
#define CORE 7.5f
#define JSON_MAX 1024
#define REFRESH_US (60LL * 1000 * 1000)

typedef struct {
    bool ok, has_team;
    float voice_min, voice_limit, voice_usd;
    int bot_tasks;
    float month_used, month_limit, week_pct;
    time_t month_resets, week_resets;
    int age_s;
} usage_t;

static struct {
    esp_gsp_handle_t ui;
    usage_face_ids_t id;
    uint16_t current;
    uint16_t *img;
    usage_t u;
    bool have;
    int64_t last_req_us;
    portMUX_TYPE lock;
    char *json;        /* written by the relay task, parsed on the UI tick */
    bool json_new, fetch_failed;
} f = { .lock = portMUX_INITIALIZER_UNLOCKED };

static inline uint16_t rgb565(int r, int g, int b)
{
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline uint16_t mix(uint16_t dst, int r, int g, int b, float a)
{
    if (a <= 0) {
        return dst;
    }
    if (a > 1) {
        a = 1;
    }
    const int dr = (dst >> 11) << 3, dg = ((dst >> 5) & 0x3F) << 2, db = (dst & 0x1F) << 3;
    return rgb565(dr + (int)((r - dr) * a), dg + (int)((g - dg) * a), db + (int)((b - db) * a));
}

static inline uint16_t add(uint16_t dst, int r, int g, int b, float a)
{
    const int dr = (dst >> 11) << 3, dg = ((dst >> 5) & 0x3F) << 2, db = (dst & 0x1F) << 3;
    return rgb565(dr + (int)(r * a), dg + (int)(g * a), db + (int)(b * a));
}

/* One ring: dark track, then a glowing arc from 12 o'clock clockwise over frac. */
static void ring(float radius, float core, float frac, int r, int g, int b)
{
    const float span = (frac < 0 ? 0 : (frac > 1 ? 1 : frac)) * 360.0f;
    const float band = core + 16.0f;
    for (int y = 0; y < FW; ++y) {
        const float dy = y + 0.5f - CY;
        if (fabsf(dy) > radius + band) {
            continue;
        }
        for (int x = 0; x < FW; ++x) {
            const float dx = x + 0.5f - CX;
            const float d = fabsf(sqrtf(dx * dx + dy * dy) - radius);
            if (d > band) {
                continue;
            }
            uint16_t *p = &f.img[y * FW + x];
            if (d < core + 0.5f) {
                *p = mix(*p, 26, 30, 38, core + 0.5f - d);
            }
            if (span <= 0) {
                continue;
            }
            float a = atan2f(dy, dx) * 180.0f / (float)M_PI + 90.0f;
            a = a < 0 ? a + 360.0f : a;
            float off = 0;
            if (a > span && span < 359.5f) {
                off = fminf(a - span, 360.0f - a) * radius * (float)M_PI / 180.0f;
            }
            const float dist = sqrtf(d * d + off * off);
            if (dist < core + 0.5f) {
                *p = mix(*p, r, g, b, core + 0.5f - dist);
            } else {
                *p = add(*p, r, g, b, 0.35f * expf(-(dist - core) / 6.0f));
            }
        }
    }
}

static void render(void)
{
    memset(f.img, 0, FW * FW * 2);
    const bool team = f.have && f.u.has_team;
    const float week = team ? f.u.week_pct / 100.0f : 0;
    const float month = team && f.u.month_limit > 0 ? f.u.month_used / f.u.month_limit : 0;
    /* Main ring: weekly included usage. Thin inner ring: extra spend vs the cap. */
    ring(R_WEEK, 10.0f, week, 90, 180, 255);
    if (month >= 0.95f) {
        ring(R_MONTH, 3.0f, month, 226, 70, 58);
    } else {
        ring(R_MONTH, 3.0f, month, 232, 160, 58);
    }
}

static void draw_cb(const esp_gsp_canvas_surface_t *s, void *ctx)
{
    (void)ctx;
    if (s->pixel_format != ESP_GSP_CANVAS_PIXEL_RGB565 || !f.img) {
        return;
    }
    uint16_t *px = s->pixels;
    const int stride = (int)(s->stride_bytes / 2);
    for (int y = 0; y < s->height; ++y) {
        memcpy(px + y * stride, f.img + (s->y + y) * FW + s->x, (size_t)s->width * 2);
    }
}

/* Days from 1970-01-01 for a civil date (UTC, no timegm on newlib). */
static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static time_t parse_iso(const cJSON *v)
{
    int Y, M, D, h, m, s;
    if (!cJSON_IsString(v) || sscanf(v->valuestring, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) {
        return 0;
    }
    return (time_t)days_from_civil(Y, M, D) * 86400 + h * 3600 + m * 60 + s;
}

static float num(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : -1;
}

static void parse(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return;
    }
    usage_t u = { .ok = true };
    u.voice_min = num(root, "voice_min");
    u.voice_limit = num(root, "voice_limit_min");
    u.voice_usd = num(root, "voice_usd");
    u.bot_tasks = (int)num(root, "bot_tasks");
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "team");
    if (cJSON_IsObject(t)) {
        u.has_team = true;
        u.month_used = num(t, "month_used_usd");
        u.month_limit = num(t, "month_limit_usd");
        u.week_pct = num(t, "week_pct");
        u.month_resets = parse_iso(cJSON_GetObjectItemCaseSensitive(t, "month_resets"));
        u.week_resets = parse_iso(cJSON_GetObjectItemCaseSensitive(t, "week_resets"));
        u.age_s = (int)num(t, "age_s");
    }
    cJSON_Delete(root);
    f.u = u;
    f.have = true;
}

static void fmt_money(char *out, size_t len, float usd)
{
    if (usd < 0) {
        snprintf(out, len, "--");
    } else if (usd >= 100) {
        snprintf(out, len, "$%d", (int)lroundf(usd));
    } else {
        snprintf(out, len, "$%.2f", usd);
    }
}

/* "2:29 PM" today, "Thu 2:29 PM" this week, else "Dec 1". */
static void fmt_when(char *out, size_t len, time_t when)
{
    const time_t now = time(NULL);
    struct tm a, b;
    localtime_r(&when, &a);
    localtime_r(&now, &b);
    char hm[16];
    strftime(hm, sizeof(hm), a.tm_min ? "%I:%M %p" : "%I %p", &a);   /* "6 PM", "2:29 PM" */
    const char *h = hm[0] == '0' ? hm + 1 : hm;
    if (a.tm_yday == b.tm_yday && a.tm_year == b.tm_year) {
        snprintf(out, len, "%s", h);
    } else if (when - now < 6 * 86400) {
        char wd[8];
        strftime(wd, sizeof(wd), "%a", &a);
        snprintf(out, len, "%s %s", wd, h);
    } else {
        strftime(out, len, "%b %e", &a);
    }
}

static void update_labels(void)
{
    char buf[96], w1[24];
    const usage_t *u = &f.u;
    if (!f.have) {
        esp_gsp_set_text(f.ui, f.id.money, "--");
        esp_gsp_set_text(f.ui, f.id.msub, "");
        esp_gsp_set_text(f.ui, f.id.week, "");
        esp_gsp_set_text(f.ui, f.id.voice, "");
        esp_gsp_set_text(f.ui, f.id.upd, f.fetch_failed ? "can't reach the relay" : "loading");
        return;
    }
    if (u->has_team) {
        snprintf(buf, sizeof(buf), "%d%%", (int)lroundf(u->week_pct));
        esp_gsp_set_text(f.ui, f.id.money, buf);
        fmt_when(w1, sizeof(w1), u->week_resets);
        snprintf(buf, sizeof(buf), "of weekly plan · resets %s", w1);
        esp_gsp_set_text(f.ui, f.id.msub, buf);
        char used[16], lim[16];
        fmt_money(used, sizeof(used), u->month_used);
        fmt_money(lim, sizeof(lim), u->month_limit);
        fmt_when(w1, sizeof(w1), u->month_resets);
        snprintf(buf, sizeof(buf), "extra %s/%s, resets %s", used, lim, w1);
        esp_gsp_set_text(f.ui, f.id.week, buf);
    } else {
        esp_gsp_set_text(f.ui, f.id.money, "--");
        esp_gsp_set_text(f.ui, f.id.msub, "team numbers not in yet");
        esp_gsp_set_text(f.ui, f.id.week, "");
    }
    snprintf(buf, sizeof(buf), "voice %d/%d min today, $%.2f", (int)lroundf(u->voice_min), (int)lroundf(u->voice_limit),
             u->voice_usd < 0 ? 0 : u->voice_usd);
    esp_gsp_set_text(f.ui, f.id.voice, buf);
    if (u->has_team && u->age_s > 3600) {
        snprintf(buf, sizeof(buf), "usage %d h old, check Mac", u->age_s / 3600);
    } else if (u->has_team) {
        snprintf(buf, sizeof(buf), "%d bot task%s, updated %dm ago", u->bot_tasks,
                 u->bot_tasks == 1 ? "" : "s", u->age_s / 60);
    } else {
        snprintf(buf, sizeof(buf), "%d bot task%s today", u->bot_tasks, u->bot_tasks == 1 ? "" : "s");
    }
    esp_gsp_set_text(f.ui, f.id.upd, buf);
}

/* Relay task: stash the body for the UI tick. */
static void on_summary(const char *json, void *ctx)
{
    (void)ctx;
    portENTER_CRITICAL(&f.lock);
    if (json) {
        strlcpy(f.json, json, JSON_MAX);
        f.json_new = true;
    } else {
        f.fetch_failed = true;
        f.json_new = true;
    }
    portEXIT_CRITICAL(&f.lock);
}

static void tick(esp_gsp_handle_t ui, void *ctx)
{
    (void)ctx;
    if (f.current != f.id.scene) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (now - f.last_req_us > REFRESH_US) {
        f.last_req_us = now;
        app_voice_request_summary();
    }
    static char local[JSON_MAX];   /* UI task: keep it off the stack */
    bool fresh = false;
    portENTER_CRITICAL(&f.lock);
    if (f.json_new) {
        f.json_new = false;
        fresh = true;
        strlcpy(local, f.json, sizeof(local));
        f.json[0] = '\0';
    }
    portEXIT_CRITICAL(&f.lock);
    if (!fresh) {
        return;
    }
    if (local[0]) {
        parse(local);
        f.fetch_failed = false;
    }
    render();
    update_labels();
    esp_gsp_canvas_invalidate(ui, f.id.canvas);
}

void usage_face_scene_changed(uint16_t scene_id)
{
    f.current = scene_id;
    if (scene_id != f.id.scene || !f.img) {
        return;
    }
    esp_gsp_canvas_set_draw_cb(f.ui, f.id.canvas, draw_cb, NULL);
    update_labels();   /* last known values until the fresh fetch lands */
    f.last_req_us = esp_timer_get_time();
    app_voice_request_summary();
}

esp_err_t usage_face_start(esp_gsp_handle_t ui, const usage_face_ids_t *ids)
{
    f.ui = ui;
    f.id = *ids;
    f.img = heap_caps_malloc(FW * FW * 2, MALLOC_CAP_SPIRAM);
    f.json = heap_caps_calloc(1, JSON_MAX, MALLOC_CAP_SPIRAM);
    if (!f.img || !f.json) {
        return ESP_ERR_NO_MEM;
    }
    render();
    app_voice_set_summary_handler(on_summary, NULL);
    esp_gsp_timer_create(ui, 500, tick, NULL);
    ESP_LOGI(TAG, "usage screen ready");
    return ESP_OK;
}
