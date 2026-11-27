// SPDX-License-Identifier: Apache-2.0
/*
 * Weather and 5-day forecast screens (one data source).
 *
 * Weather screen: a sky-tinted background (time of day and conditions), a drawn
 * condition icon and the current temperature on top, a row with the next five
 * hours (time, small icon, temperature) and the pollen count.
 * Forecast screen: today and the next four days, each with an icon, chance of
 * rain and a low-high bar on one temperature scale for the whole week.
 *
 * Data comes from the relay's GET /weather (Open-Meteo at the Wi-Fi network's
 * location), fetched on the voice relay task like the usage summary. Refreshes
 * on arrival, on entering the screen when older than 10 min, and every 10 min
 * while visible.
 */
#include "clock_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_voice.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "weather_face";

#define FW 480
#define CX 240.0f
#define CY 240.0f
#define ICON_X 240.0f
#define ICON_Y 80.0f
#define ROW_Y 308.0f
#define ROW_DX 78.0f
#define ROW_N 5
#define ROW_SCALE 0.52f
#define JSON_MAX 2048
#define HOURS 12
#define WEEK 5
#define F_ROW0 106.0f
#define F_ROW_DY 64.0f
#define F_ICON_X 172.0f
#define F_BAR_X0 312.0f
#define F_BAR_X1 354.0f
#define REFRESH_US (10LL * 60 * 1000 * 1000)
#define BOOT_FETCH_US (20LL * 1000 * 1000)
#define RETRY_US (60LL * 1000 * 1000)

typedef struct {
    int h, t, code, pop;
    bool day;
} hour_t;

typedef struct {
    char d[6];
    int hi, lo, code;
} day_t;

typedef struct {
    char place[32], unit, wind_unit[8];
    int t, feels, code, wind;
    bool is_day;
    int hi, lo, pop;
    char rise[6], set[6];
    hour_t hours[HOURS];
    int n_hours;
    day_t week[WEEK];
    int week_pop[WEEK];
    int n_week;
    float pollen, pollen_tmr;   /* Pollen.com index 0-12; < 0 = none */
    char pollen_lvl[12], pollen_top[40];
} wx_t;

static struct {
    esp_gsp_handle_t ui;
    weather_face_ids_t id;
    uint16_t current;
    uint16_t *img, *img2;   /* weather, forecast */
    wx_t w;
    bool have, fetch_failed;
    int64_t got_us, last_req_us;
    portMUX_TYPE lock;
    char *json;   /* written by the relay task, parsed on the UI tick */
    bool json_new;
} f = { .lock = portMUX_INITIALIZER_UNLOCKED };

/* ---- pixels ---------------------------------------------------------------- */

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

typedef struct {
    int r, g, b;
} rgb_t;

static uint16_t *dst;   /* canvas being drawn (weather or forecast) */

/* ---- icon (signed distances, anti-aliased) --------------------------------- */

typedef enum { K_CLEAR, K_PARTLY, K_CLOUD, K_FOG, K_RAIN, K_SNOW, K_STORM } kind_t;

static kind_t kind_of(int code)
{
    if (code <= 1) return K_CLEAR;
    if (code == 2) return K_PARTLY;
    if (code == 3) return K_CLOUD;
    if (code <= 48) return K_FOG;
    if (code <= 67 || (code >= 80 && code <= 82)) return K_RAIN;
    if (code <= 77 || code == 85 || code == 86) return K_SNOW;
    return K_STORM;
}

static const char *cond_name(int code)
{
    switch (code) {
    case 0: return "Clear";
    case 1: return "Mostly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    default: return code >= 95 ? "Thunderstorms" : "--";
    }
}

static float sd_circle(float x, float y, float cx, float cy, float r)
{
    return hypotf(x - cx, y - cy) - r;
}

static float sd_capsule(float x, float y, float ax, float ay, float bx, float by, float r)
{
    const float px = x - ax, py = y - ay, vx = bx - ax, vy = by - ay;
    float h = (px * vx + py * vy) / (vx * vx + vy * vy);
    h = h < 0 ? 0 : (h > 1 ? 1 : h);
    return hypotf(px - vx * h, py - vy * h) - r;
}

/* Cloud: three puffs over a flat rounded base, centred near (cx, cy). */
static float sd_cloud(float x, float y, float cx, float cy, float s)
{
    float d = sd_circle(x, y, cx - 20 * s, cy + 4 * s, 14 * s);
    d = fminf(d, sd_circle(x, y, cx + 1 * s, cy - 8 * s, 20 * s));
    d = fminf(d, sd_circle(x, y, cx + 22 * s, cy + 5 * s, 13 * s));
    d = fminf(d, sd_capsule(x, y, cx - 20 * s, cy + 9 * s, cx + 22 * s, cy + 9 * s, 9 * s));
    return d;
}

static float sd_sun(float x, float y, float cx, float cy, float s)
{
    float d = sd_circle(x, y, cx, cy, 17 * s);
    for (int i = 0; i < 8; ++i) {
        const float a = i * (float)M_PI / 4.0f;
        const float c = cosf(a), sn = sinf(a);
        d = fminf(d, sd_capsule(x, y, cx + c * 25 * s, cy + sn * 25 * s, cx + c * 33 * s, cy + sn * 33 * s, 2.6f * s));
    }
    return d;
}

static float sd_moon(float x, float y, float cx, float cy, float s)
{
    return fmaxf(sd_circle(x, y, cx, cy, 20 * s), -sd_circle(x, y, cx + 11 * s, cy - 8 * s, 17 * s));
}

typedef float (*sdf_fn)(float x, float y, float cx, float cy, float s);

/* Visit the pixels of an icon box centred at (cx, cy) at scale s. */
#define ICON_BOX(cx, cy, s)                                                                 \
    for (int y = (int)((cy) - 56 * (s)); y < (int)((cy) + 58 * (s)); ++y)                   \
        for (int x = (int)((cx) - 66 * (s)); x < (int)((cx) + 66 * (s)); ++x)

/* Paint one shape; `ring` > 0 cuts a dark outline first so a cloud reads
 * cleanly over the sun. */
static void paint(sdf_fn fn, float cx, float cy, float s, rgb_t c, float ring, float bx, float by, float bs)
{
    ICON_BOX(bx, by, bs) {
        const float d = fn(x + 0.5f, y + 0.5f, cx, cy, s);
        uint16_t *p = &dst[y * FW + x];
        if (ring > 0) {
            *p = mix(*p, 0, 0, 0, 0.7f * (ring * s + 0.5f - d));
        }
        *p = mix(*p, c.r, c.g, c.b, 0.5f - d);
    }
}

static void paint_drops(float cx, float cy, float s, rgb_t c, bool snow)
{
    ICON_BOX(cx, cy, s) {
        const float px = x + 0.5f, py = y + 0.5f;
        float d = 1e9f;
        for (int i = -1; i <= 1; ++i) {
            const float bx = cx + i * 16.0f * s, by = cy + 26.0f * s;
            if (snow) {
                d = fminf(d, sd_circle(px, py, bx, by + (i ? 0 : 8) * s, 3.6f * s));
                d = fminf(d, sd_circle(px, py, bx - 8 * s, by + (i ? 14 : 22) * s, 3.0f * s));
            } else {
                d = fminf(d, sd_capsule(px, py, bx + 4 * s, by, bx - 4 * s, by + 18 * s, 2.4f * s));
            }
        }
        dst[y * FW + x] = mix(dst[y * FW + x], c.r, c.g, c.b, 0.5f - d);
    }
}

static void paint_bolt(float cx, float cy, float s)
{
    const float x0 = cx + 4 * s, y0 = cy + 18 * s, w = 2.8f * s;
    ICON_BOX(cx, cy, s) {
        const float px = x + 0.5f, py = y + 0.5f;
        float d = sd_capsule(px, py, x0, y0, x0 - 10 * s, y0 + 15 * s, w);
        d = fminf(d, sd_capsule(px, py, x0 - 10 * s, y0 + 15 * s, x0 + 2 * s, y0 + 15 * s, w));
        d = fminf(d, sd_capsule(px, py, x0 + 2 * s, y0 + 15 * s, x0 - 8 * s, y0 + 32 * s, w));
        dst[y * FW + x] = mix(dst[y * FW + x], 255, 200, 60, 0.5f - d);
    }
}

static void paint_fog(float cx, float cy, float s)
{
    ICON_BOX(cx, cy, s) {
        const float px = x + 0.5f, py = y + 0.5f, w = 4.0f * s;
        float d = sd_capsule(px, py, cx - 34 * s, cy - 14 * s, cx + 30 * s, cy - 14 * s, w);
        d = fminf(d, sd_capsule(px, py, cx - 26 * s, cy + 2 * s, cx + 36 * s, cy + 2 * s, w));
        d = fminf(d, sd_capsule(px, py, cx - 36 * s, cy + 18 * s, cx + 22 * s, cy + 18 * s, w));
        dst[y * FW + x] = mix(dst[y * FW + x], 170, 178, 192, 0.5f - d);
    }
}

/* One condition icon at (cx, cy), scale s (1 = the big one). */
static void draw_icon(int code, bool day, float cx, float cy, float s)
{
    const rgb_t sun = { 255, 196, 64 }, moon = { 222, 228, 242 };
    const rgb_t light = { 214, 220, 230 }, grey = { 150, 158, 174 }, dark = { 112, 118, 136 };
    const rgb_t rain = { 90, 160, 255 }, snow = { 240, 244, 255 };
    const sdf_fn orb = day ? sd_sun : sd_moon;
    const rgb_t orb_c = day ? sun : moon;
    switch (kind_of(code)) {
    case K_CLEAR:
        paint(orb, cx, cy, 1.25f * s, orb_c, 0, cx, cy, s);
        break;
    case K_PARTLY:
        paint(orb, cx - 16 * s, cy - 12 * s, 0.95f * s, orb_c, 0, cx, cy, s);
        paint(sd_cloud, cx + 8 * s, cy + 10 * s, s, light, 3.0f, cx, cy, s);
        break;
    case K_CLOUD:
        paint(sd_cloud, cx, cy, 1.25f * s, grey, 0, cx, cy, s);
        break;
    case K_FOG:
        paint_fog(cx, cy, s);
        break;
    case K_RAIN:
        paint(sd_cloud, cx, cy - 12 * s, 1.1f * s, grey, 0, cx, cy, s);
        paint_drops(cx, cy, s, rain, false);
        break;
    case K_SNOW:
        paint(sd_cloud, cx, cy - 12 * s, 1.1f * s, light, 0, cx, cy, s);
        paint_drops(cx, cy, s, snow, true);
        break;
    case K_STORM:
        paint(sd_cloud, cx, cy - 12 * s, 1.1f * s, dark, 0, cx, cy, s);
        paint_bolt(cx, cy, s);
        break;
    }
}

/* Sky colour at the top of the screen for the time of day and conditions. */
static rgb_t sky_top(void)
{
    const kind_t k = kind_of(f.w.code);
    if (!f.have) {
        return (rgb_t){ 14, 18, 28 };
    }
    if (!f.w.is_day) {
        return k <= K_PARTLY ? (rgb_t){ 12, 20, 52 } : (rgb_t){ 20, 24, 34 };
    }
    rgb_t c;
    switch (k) {
    case K_CLEAR: c = (rgb_t){ 30, 84, 160 }; break;
    case K_PARTLY: c = (rgb_t){ 40, 74, 128 }; break;
    case K_SNOW: c = (rgb_t){ 66, 76, 98 }; break;
    case K_RAIN: case K_STORM: c = (rgb_t){ 36, 46, 66 }; break;
    default: c = (rgb_t){ 56, 64, 80 }; break;
    }
    /* Warm it in the hour around sunrise and sunset. */
    int sh, sm, rh, rm;
    const time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    const int mins = lt.tm_hour * 60 + lt.tm_min;
    float warm = 0;
    if (sscanf(f.w.set, "%d:%d", &sh, &sm) == 2) {
        warm = fmaxf(warm, 1.0f - abs(mins - (sh * 60 + sm)) / 60.0f);
    }
    if (sscanf(f.w.rise, "%d:%d", &rh, &rm) == 2) {
        warm = fmaxf(warm, 1.0f - abs(mins - (rh * 60 + rm)) / 60.0f);
    }
    if (warm > 0) {
        warm *= 0.65f;
        c = (rgb_t){ c.r + (int)((170 - c.r) * warm), c.g + (int)((86 - c.g) * warm), c.b + (int)((60 - c.b) * warm) };
    }
    return c;
}

/* Pollen level colour, green (low) to red (high), on Pollen.com's 0-12 scale. */
static rgb_t pollen_color(float i)
{
    static const rgb_t lv[5] = {
        { 90, 190, 100 }, { 170, 200, 70 }, { 235, 185, 50 }, { 240, 120, 45 }, { 230, 60, 55 },
    };
    return lv[i < 2.5f ? 0 : i < 4.9f ? 1 : i < 7.3f ? 2 : i < 9.7f ? 3 : 4];
}

/* Rounded pill behind the pollen line, tinted by level with a brighter rim. */
static void draw_pollen_pill(void)
{
    if (f.w.pollen < 0) {
        return;
    }
    const rgb_t c = pollen_color(f.w.pollen);
    const float x0 = 78, x1 = 402, cy = 385, r = 14;
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); ++y) {
        for (int x = (int)(x0 - r - 1); x <= (int)(x1 + r + 1); ++x) {
            const float d = sd_capsule(x + 0.5f, y + 0.5f, x0, cy, x1, cy, r);
            uint16_t *p = &dst[y * FW + x];
            *p = mix(*p, c.r, c.g, c.b, 0.30f * (0.5f - d));
            *p = mix(*p, c.r, c.g, c.b, 0.85f * (0.75f - fabsf(d + 0.75f)));
        }
    }
}

static void sky(void)
{
    const rgb_t top = sky_top();
    for (int y = 0; y < FW; ++y) {
        float a = 1.0f - y / 360.0f;
        a = a <= 0 ? 0 : powf(a, 1.3f);
        const uint16_t c = rgb565((int)(top.r * a), (int)(top.g * a), (int)(top.b * a));
        for (int x = 0; x < FW; ++x) {
            dst[y * FW + x] = c;
        }
    }
}

static void render(void)
{
    dst = f.img;
    sky();
    if (!f.have) {
        return;
    }
    draw_icon(f.w.code, f.w.is_day, ICON_X, ICON_Y, 0.85f);
    draw_pollen_pill();
    for (int i = 0; i < ROW_N && i + 1 < f.w.n_hours; ++i) {
        const hour_t *h = &f.w.hours[i + 1];
        draw_icon(h->code, h->day, CX + (i - 2) * ROW_DX, ROW_Y, ROW_SCALE);
    }
}

/* Temperature colour on a Fahrenheit scale (Celsius is converted first). */
static rgb_t temp_color(float t)
{
    static const struct {
        float t;
        rgb_t c;
    } stops[] = {
        { 15, { 170, 130, 255 } }, { 32, { 90, 165, 255 } }, { 50, { 60, 200, 210 } },
        { 65, { 120, 210, 110 } }, { 77, { 245, 195, 60 } }, { 88, { 245, 125, 50 } },
        { 100, { 230, 55, 55 } },
    };
    const int n = (int)(sizeof(stops) / sizeof(stops[0]));
    const float tf = f.w.unit == 'C' ? t * 1.8f + 32.0f : t;
    if (tf <= stops[0].t) {
        return stops[0].c;
    }
    for (int i = 1; i < n; ++i) {
        if (tf <= stops[i].t) {
            const float k = (tf - stops[i - 1].t) / (stops[i].t - stops[i - 1].t);
            const rgb_t a = stops[i - 1].c, b = stops[i].c;
            return (rgb_t){ a.r + (int)((b.r - a.r) * k), a.g + (int)((b.g - a.g) * k), a.b + (int)((b.b - a.b) * k) };
        }
    }
    return stops[n - 1].c;
}

/* Forecast rows: icon, and a low-high bar on one scale for the whole week,
 * coloured by temperature; today's bar carries a dot at the current temperature. */
static void render_forecast(void)
{
    dst = f.img2;
    sky();
    if (!f.have || !f.w.n_week) {
        return;
    }
    int wmin = f.w.week[0].lo, wmax = f.w.week[0].hi;
    for (int i = 0; i < f.w.n_week; ++i) {
        wmin = f.w.week[i].lo < wmin ? f.w.week[i].lo : wmin;
        wmax = f.w.week[i].hi > wmax ? f.w.week[i].hi : wmax;
    }
    const float span = wmax > wmin ? (float)(wmax - wmin) : 1.0f;
    const float bw = F_BAR_X1 - F_BAR_X0, r = 5.0f;
    for (int i = 0; i < f.w.n_week; ++i) {
        const day_t *d = &f.w.week[i];
        const float yc = F_ROW0 + i * F_ROW_DY;
        draw_icon(d->code, true, F_ICON_X, yc, 0.5f);
        const float xa = F_BAR_X0 + (d->lo - wmin) / span * bw;
        const float xb = F_BAR_X0 + (d->hi - wmin) / span * bw;
        for (int y = (int)(yc - r - 2); y <= (int)(yc + r + 2); ++y) {
            for (int x = (int)(F_BAR_X0 - r - 2); x <= (int)(F_BAR_X1 + r + 2); ++x) {
                const float px = x + 0.5f, py = y + 0.5f;
                uint16_t *p = &dst[y * FW + x];
                *p = mix(*p, 40, 46, 58, 0.5f - sd_capsule(px, py, F_BAR_X0, yc, F_BAR_X1, yc, r));
                const rgb_t c = temp_color(wmin + (px - F_BAR_X0) / bw * span);
                *p = mix(*p, c.r, c.g, c.b, 0.5f - sd_capsule(px, py, xa, yc, xb, yc, r));
            }
        }
        if (i == 0) {
            float t = (float)f.w.t;
            t = t < wmin ? wmin : (t > wmax ? wmax : t);
            const float xn = F_BAR_X0 + (t - wmin) / span * bw;
            for (int y = (int)(yc - 7); y <= (int)(yc + 7); ++y) {
                for (int x = (int)(xn - 7); x <= (int)(xn + 7); ++x) {
                    const float dd = sd_circle(x + 0.5f, y + 0.5f, xn, yc, 5.0f);
                    uint16_t *p = &dst[y * FW + x];
                    *p = mix(*p, 0, 0, 0, 0.8f * (2.0f - dd));
                    *p = mix(*p, 255, 255, 255, 0.5f - dd);
                }
            }
        }
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

static void draw_cb2(const esp_gsp_canvas_surface_t *s, void *ctx)
{
    (void)ctx;
    if (s->pixel_format != ESP_GSP_CANVAS_PIXEL_RGB565 || !f.img2) {
        return;
    }
    uint16_t *px = s->pixels;
    const int stride = (int)(s->stride_bytes / 2);
    for (int y = 0; y < s->height; ++y) {
        memcpy(px + y * stride, f.img2 + (s->y + y) * FW + s->x, (size_t)s->width * 2);
    }
}

/* ---- data ------------------------------------------------------------------ */

static int inum(const cJSON *o, const char *k, int dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (int)lround(v->valuedouble) : dflt;
}

static void str(const cJSON *o, const char *k, char *out, size_t len)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    snprintf(out, len, "%s", cJSON_IsString(v) ? v->valuestring : "");
}

static bool parse(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"))) {
        cJSON_Delete(root);
        return false;
    }
    wx_t w = { 0 };
    str(root, "place", w.place, sizeof(w.place));
    char u[4];
    str(root, "unit", u, sizeof(u));
    w.unit = u[0] == 'C' ? 'C' : 'F';
    str(root, "wind_unit", w.wind_unit, sizeof(w.wind_unit));
    const cJSON *now = cJSON_GetObjectItemCaseSensitive(root, "now");
    w.t = inum(now, "t", 0);
    w.feels = inum(now, "feels", w.t);
    w.code = inum(now, "c", 0);
    w.wind = inum(now, "wind", 0);
    w.is_day = inum(now, "d", 1) != 0;
    const cJSON *today = cJSON_GetObjectItemCaseSensitive(root, "today");
    w.hi = inum(today, "hi", w.t);
    w.lo = inum(today, "lo", w.t);
    w.pop = inum(today, "p", 0);
    str(today, "rise", w.rise, sizeof(w.rise));
    str(today, "set", w.set, sizeof(w.set));
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "hours")) {
        if (w.n_hours >= HOURS) {
            break;
        }
        hour_t *h = &w.hours[w.n_hours++];
        h->h = inum(it, "h", 0);
        h->t = inum(it, "t", 0);
        h->code = inum(it, "c", 0);
        h->pop = inum(it, "p", 0);
        h->day = inum(it, "d", 1) != 0;
    }
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "week")) {
        if (w.n_week >= WEEK) {
            break;
        }
        day_t *d = &w.week[w.n_week];
        str(it, "d", d->d, sizeof(d->d));
        d->hi = inum(it, "hi", 0);
        d->lo = inum(it, "lo", 0);
        d->code = inum(it, "c", 0);
        w.week_pop[w.n_week++] = inum(it, "p", 0);
    }
    w.pollen = w.pollen_tmr = -1;
    const cJSON *pol = cJSON_GetObjectItemCaseSensitive(root, "pollen");
    if (cJSON_IsObject(pol)) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(pol, "i");
        w.pollen = cJSON_IsNumber(v) ? (float)v->valuedouble : -1;
        v = cJSON_GetObjectItemCaseSensitive(pol, "tmr");
        w.pollen_tmr = cJSON_IsNumber(v) ? (float)v->valuedouble : -1;
        str(pol, "lvl", w.pollen_lvl, sizeof(w.pollen_lvl));
        size_t n = 0;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(pol, "top")) {
            if (cJSON_IsString(it) && n < sizeof(w.pollen_top)) {
                n += (size_t)snprintf(w.pollen_top + n, sizeof(w.pollen_top) - n, "%s%s", n ? ", " : "",
                                      it->valuestring);
            }
        }
    }
    cJSON_Delete(root);
    f.w = w;
    f.have = true;
    return true;
}

/* "Pollen 3.5 low-med · ragweed, juniper"; the pill behind it (render) shows the level. */
static void set_pollen(const wx_t *w)
{
    char buf[96];
    if (w->pollen < 0) {
        esp_gsp_set_text(f.ui, f.id.pollen, "");
        return;
    }
    snprintf(buf, sizeof(buf), "Pollen %.1f %s%s%s", w->pollen, w->pollen_lvl, w->pollen_top[0] ? " \u00b7 " : "",
             w->pollen_top);
    esp_gsp_set_text(f.ui, f.id.pollen, buf);
}

static void fmt_age(char *buf, size_t len);

static void update_labels(void)
{
    char buf[96], t[24];
    if (!f.have) {
        esp_gsp_set_text(f.ui, f.id.temp, "--");
        esp_gsp_set_text(f.ui, f.id.cond, f.fetch_failed ? "can't reach the relay" : "loading");
        esp_gsp_set_text(f.ui, f.id.pollen, "");
        for (int i = 0; i < ROW_N; ++i) {
            esp_gsp_set_text(f.ui, f.id.hour[i], "");
            esp_gsp_set_text(f.ui, f.id.htemp[i], "");
        }
        esp_gsp_set_text(f.ui, f.id.upd, "");
        return;
    }
    const wx_t *w = &f.w;
    snprintf(buf, sizeof(buf), "%d°", w->t);
    esp_gsp_set_text(f.ui, f.id.temp, buf);
    /* "Clear · feels 45° · H 67° L 34°" (feels only when it differs). */
    char feels[24] = "";
    if (abs(w->feels - w->t) >= 3) {
        snprintf(feels, sizeof(feels), " \u00b7 feels %d\u00b0", w->feels);
    }
    snprintf(buf, sizeof(buf), "%s%s \u00b7 H %d\u00b0 L %d\u00b0", cond_name(w->code), feels, w->hi, w->lo);
    esp_gsp_set_text(f.ui, f.id.cond, buf);
    for (int i = 0; i < ROW_N; ++i) {
        if (i + 1 >= w->n_hours) {
            esp_gsp_set_text(f.ui, f.id.hour[i], "");
            esp_gsp_set_text(f.ui, f.id.htemp[i], "");
            continue;
        }
        const hour_t *h = &w->hours[i + 1];
        if (h->pop >= 30) {
            snprintf(t, sizeof(t), "%d%s %d%%", h->h % 12 ? h->h % 12 : 12, h->h < 12 ? "am" : "pm", h->pop);
        } else {
            snprintf(t, sizeof(t), "%d%s", h->h % 12 ? h->h % 12 : 12, h->h < 12 ? "am" : "pm");
        }
        esp_gsp_set_text(f.ui, f.id.hour[i], t);
        snprintf(t, sizeof(t), "%d\u00b0", h->t);
        esp_gsp_set_text(f.ui, f.id.htemp[i], t);
    }
    set_pollen(w);
    fmt_age(buf, sizeof(buf));
    esp_gsp_set_text(f.ui, f.id.upd, buf);
}

static void fmt_age(char *buf, size_t len)
{
    const int age_min = (int)((esp_timer_get_time() - f.got_us) / 60000000LL);
    if (f.fetch_failed && age_min >= 30) {
        snprintf(buf, len, "%s \u00b7 %d min old, relay unreachable", f.w.place, age_min);
    } else if (age_min >= 1) {
        snprintf(buf, len, "%s \u00b7 updated %d min ago", f.w.place, age_min);
    } else {
        snprintf(buf, len, "%s \u00b7 just updated", f.w.place);
    }
}

static void update_forecast_labels(void)
{
    char buf[64];
    for (int i = 0; i < WEEK; ++i) {
        const bool ok = f.have && i < f.w.n_week;
        const day_t *d = &f.w.week[i];
        esp_gsp_set_text(f.ui, f.id.fday[i], ok ? d->d : "");
        snprintf(buf, sizeof(buf), "%d%%", ok ? f.w.week_pop[i] : 0);
        esp_gsp_set_text(f.ui, f.id.fpop[i], ok && f.w.week_pop[i] >= 20 ? buf : "");
        snprintf(buf, sizeof(buf), "%d\u00b0", d->lo);
        esp_gsp_set_text(f.ui, f.id.flo[i], ok ? buf : "");
        snprintf(buf, sizeof(buf), "%d\u00b0", d->hi);
        esp_gsp_set_text(f.ui, f.id.fhi[i], ok ? buf : "");
    }
    if (f.have) {
        fmt_age(buf, sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "%s", f.fetch_failed ? "can't reach the relay" : "loading");
    }
    esp_gsp_set_text(f.ui, f.id.fupd, buf);
}

/* Relay task: stash the body for the UI tick (NULL = fetch failed). */
static void on_weather(const char *json, void *ctx)
{
    (void)ctx;
    portENTER_CRITICAL(&f.lock);
    if (json) {
        strlcpy(f.json, json, JSON_MAX);
    } else {
        f.json[0] = '\0';
    }
    f.json_new = true;
    portEXIT_CRITICAL(&f.lock);
}

static void request(int64_t now)
{
    f.last_req_us = now;
    app_voice_request_weather();
}

static void tick(esp_gsp_handle_t ui, void *ctx)
{
    (void)ctx;
    const int64_t now = esp_timer_get_time();
    const bool on_weather = f.current == f.id.scene, on_forecast = f.current == f.id.fscene;
    const bool visible = on_weather || on_forecast;
    if (!f.last_req_us && now > BOOT_FETCH_US) {
        request(now);   /* once after boot, so the first look is instant */
    } else if (visible && now - f.last_req_us > (f.have && !f.fetch_failed ? REFRESH_US : RETRY_US)) {
        request(now);
    }
    static char local[JSON_MAX];   /* UI task: keep it off the stack */
    bool fresh = false;
    portENTER_CRITICAL(&f.lock);
    if (f.json_new) {
        f.json_new = false;
        fresh = true;
        strlcpy(local, f.json, sizeof(local));
    }
    portEXIT_CRITICAL(&f.lock);
    if (fresh) {
        if (local[0] && parse(local)) {
            f.fetch_failed = false;
            f.got_us = now;
        } else {
            f.fetch_failed = true;
            ESP_LOGW(TAG, "weather fetch failed");
        }
        if (on_weather) {
            render();
            esp_gsp_canvas_invalidate(ui, f.id.canvas);
        } else if (on_forecast) {
            render_forecast();
            esp_gsp_canvas_invalidate(ui, f.id.fcanvas);
        }
    }
    /* Labels every ~30 s while visible keep "updated N min ago" honest. */
    static int64_t last_lbl;
    if (visible && (fresh || now - last_lbl > 30LL * 1000 * 1000)) {
        last_lbl = now;
        if (on_weather) {
            update_labels();
        } else {
            update_forecast_labels();
        }
    }
}

void weather_face_scene_changed(uint16_t scene_id)
{
    f.current = scene_id;
    if (scene_id == f.id.scene && f.img) {
        esp_gsp_canvas_set_draw_cb(f.ui, f.id.canvas, draw_cb, NULL);
        render();          /* last known data until a fresh fetch lands */
        update_labels();
    } else if (scene_id == f.id.fscene && f.img2) {
        esp_gsp_canvas_set_draw_cb(f.ui, f.id.fcanvas, draw_cb2, NULL);
        render_forecast();
        update_forecast_labels();
    } else {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (!f.have || now - f.got_us > REFRESH_US) {
        request(now);
    }
}

esp_err_t weather_face_start(esp_gsp_handle_t ui, const weather_face_ids_t *ids)
{
    f.ui = ui;
    f.id = *ids;
    f.img = heap_caps_malloc(FW * FW * 2, MALLOC_CAP_SPIRAM);
    f.img2 = heap_caps_malloc(FW * FW * 2, MALLOC_CAP_SPIRAM);
    f.json = heap_caps_calloc(1, JSON_MAX, MALLOC_CAP_SPIRAM);
    if (!f.img || !f.img2 || !f.json) {
        return ESP_ERR_NO_MEM;
    }
    render();
    app_voice_set_weather_handler(on_weather, NULL);
    esp_gsp_timer_create(ui, 500, tick, NULL);
    ESP_LOGI(TAG, "weather screen ready");
    return ESP_OK;
}
