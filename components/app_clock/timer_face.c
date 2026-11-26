// SPDX-License-Identifier: Apache-2.0
/*
 * Countdown timer screen. The timer itself lives in app_alarm (so voice
 * timers show here and ring through the normal alarm path); this file draws
 * it and handles touch.
 *
 * Canvas: a cached base (dark track + minute ticks) plus, per frame, the
 * glowing blue arc for the time remaining (from 12 o'clock clockwise), a
 * white knob at its end and two small eyes that watch the knob and blink.
 * The digits and subtitle are GSP labels on top of the canvas.
 *
 * Touch: drag around the ring to set (one turn = 60 min, keeps going to 99),
 * tap the centre to start/pause (or stop a finished timer), tap "reset" below.
 */
#include "clock_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_alarm.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "timer_face";

#define FW 480
#define CX 240.0f
#define CY 240.0f
#define R_ARC 200.0f
#define CORE 8.5f        /* half-width of the arc */
#define GLOW 22.0f       /* glow reach beyond the core */

#define COL_ARC_R 90
#define COL_ARC_G 180
#define COL_ARC_B 255

static struct {
    esp_gsp_handle_t ui;
    uint16_t scene, current, canvas, digits, sub, reset;
    uint16_t *base;
    /* drag-to-set */
    bool pressed, dragging;
    int32_t px, py;
    float last_deg, acc_deg;   /* unwrapped drag angle; 6 degrees = 1 minute */
    /* label cache */
    char shown_digits[16], shown_sub[64], shown_reset[16];
    /* frame snapshot used by the draw callback */
    float frac, knob_deg, pulse, look_x, look_y;
    bool blink, has_timer;
} t;

static inline uint16_t rgb565(int r, int g, int b)
{
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline void unpack(uint16_t p, int *r, int *g, int *b)
{
    *r = (p >> 11) << 3;
    *g = ((p >> 5) & 0x3F) << 2;
    *b = (p & 0x1F) << 3;
}

static inline uint16_t mix(uint16_t dst, int r, int g, int b, float a)
{
    if (a <= 0) {
        return dst;
    }
    if (a > 1) {
        a = 1;
    }
    int dr, dg, db;
    unpack(dst, &dr, &dg, &db);
    return rgb565(dr + (int)((r - dr) * a), dg + (int)((g - dg) * a), db + (int)((b - db) * a));
}

/* Additive glow so it brightens the dark track instead of covering it. */
static inline uint16_t add(uint16_t dst, int r, int g, int b, float a)
{
    int dr, dg, db;
    unpack(dst, &dr, &dg, &db);
    return rgb565(dr + (int)(r * a), dg + (int)(g * a), db + (int)(b * a));
}

static float deg_of(float x, float y)
{
    float d = atan2f(y - CY, x - CX) * 180.0f / (float)M_PI + 90.0f;
    return d < 0 ? d + 360.0f : d;
}

static void build_base(void)
{
    for (int y = 0; y < FW; ++y) {
        for (int x = 0; x < FW; ++x) {
            const float dx = x + 0.5f - CX, dy = y + 0.5f - CY;
            const float r = sqrtf(dx * dx + dy * dy);
            uint16_t p = 0;
            /* dark track under the arc */
            const float d = fabsf(r - R_ARC) - CORE;
            if (d < 0.5f) {
                p = mix(p, 34, 38, 48, 0.5f - d);
            }
            /* minute ticks inside the ring, longer every 5 */
            if (r > 158 && r < 182) {
                const float a = deg_of(x + 0.5f, y + 0.5f);
                const float m = fmodf(a + 3.0f, 6.0f) - 3.0f;          /* degrees from nearest tick */
                const int idx = (int)lroundf(a / 6.0f) % 60;
                const bool major = idx % 5 == 0;
                const float r0 = major ? 163.0f : 170.0f;
                if (r > r0 && r < 180.0f) {
                    const float w = (major ? 0.9f : 0.55f) - fabsf(m) * r * (float)M_PI / 180.0f;
                    if (w > -0.5f) {
                        p = mix(p, major ? 120 : 70, major ? 126 : 74, major ? 140 : 84, w + 0.5f);
                    }
                }
            }
            t.base[y * FW + x] = p;
        }
    }
}

static void draw_capsule(uint16_t *px, int stride, int x0, int y0, int w, int h,
                         float ax, float ay, float bx, float by, float rad)
{
    const int minx = (int)fmaxf(fminf(ax, bx) - rad - 2, x0), maxx = (int)fminf(fmaxf(ax, bx) + rad + 2, x0 + w - 1);
    const int miny = (int)fmaxf(fminf(ay, by) - rad - 2, y0), maxy = (int)fminf(fmaxf(ay, by) + rad + 2, y0 + h - 1);
    const float bax = bx - ax, bay = by - ay, bb = bax * bax + bay * bay;
    for (int y = miny; y <= maxy; ++y) {
        for (int x = minx; x <= maxx; ++x) {
            const float pax = x + 0.5f - ax, pay = y + 0.5f - ay;
            float hh = bb > 0 ? (pax * bax + pay * bay) / bb : 0;
            hh = hh < 0 ? 0 : (hh > 1 ? 1 : hh);
            const float qx = pax - bax * hh, qy = pay - bay * hh;
            const float a = 0.5f - (sqrtf(qx * qx + qy * qy) - rad);
            if (a > 0) {
                uint16_t *p = &px[(y - y0) * stride + (x - x0)];
                *p = mix(*p, 255, 255, 255, a);
            }
        }
    }
}

static void draw_cb(const esp_gsp_canvas_surface_t *s, void *ctx)
{
    (void)ctx;
    uint16_t *px = s->pixels;
    const int stride = (int)(s->stride_bytes / 2);
    const int x0 = s->x, y0 = s->y, w = s->width, h = s->height;
    if (s->pixel_format != ESP_GSP_CANVAS_PIXEL_RGB565) {
        for (int y = 0; y < h; ++y) {
            memset((uint8_t *)s->pixels + y * s->stride_bytes, 0, (size_t)w * 3);
        }
        return;
    }
    for (int y = 0; y < h; ++y) {
        memcpy(px + y * stride, t.base + (y0 + y) * FW + x0, (size_t)w * 2);
    }
    /* Glowing arc: 0 .. frac*360 degrees clockwise from 12. */
    const float span = t.frac * 360.0f;
    const float glow_gain = 0.55f + 0.45f * t.pulse;
    const float band = CORE + GLOW;
    for (int y = y0; y < y0 + h; ++y) {
        const float dy = y + 0.5f - CY;
        if (fabsf(dy) > R_ARC + band) {
            continue;
        }
        uint16_t *row = px + (y - y0) * stride - x0;
        for (int x = x0; x < x0 + w; ++x) {
            const float dx = x + 0.5f - CX;
            const float r = sqrtf(dx * dx + dy * dy);
            const float d = fabsf(r - R_ARC);
            if (d > band || span <= 0) {
                continue;
            }
            const float a = deg_of(x + 0.5f, y + 0.5f);
            /* distance along the arc outside [0, span] (in px), for round ends */
            float off = 0;
            if (a > span) {
                const float past = a - span, before = 360.0f - a;
                off = fminf(past, before) * R_ARC * (float)M_PI / 180.0f;
                if (span >= 359.5f) {
                    off = 0;
                }
            }
            const float dist = sqrtf(d * d + off * off);
            if (dist < CORE + 0.5f) {
                row[x] = mix(row[x], COL_ARC_R, COL_ARC_G, COL_ARC_B, CORE + 0.5f - dist);
            } else {
                const float g = glow_gain * 0.6f * expf(-(dist - CORE) / 7.0f);
                row[x] = add(row[x], COL_ARC_R, COL_ARC_G, COL_ARC_B, g);
            }
        }
    }
    /* Knob at the end of the arc */
    const float ka = (t.knob_deg - 90.0f) * (float)M_PI / 180.0f;
    const float kx = CX + R_ARC * cosf(ka), ky = CY + R_ARC * sinf(ka);
    draw_capsule(px, stride, x0, y0, w, h, kx, ky, kx, ky, 15.0f);
    /* Eyes: upright capsules that look toward the knob, with blinks */
    const float lx = t.look_x * 7.0f, ly = t.look_y * 6.0f;
    for (int e = 0; e < 2; ++e) {
        const float ex = (e ? 274.0f : 206.0f) + lx, ey = 140.0f + ly;
        if (t.blink) {
            draw_capsule(px, stride, x0, y0, w, h, ex - 12, ey + 6, ex + 12, ey + 6, 3.5f);
        } else {
            draw_capsule(px, stride, x0, y0, w, h, ex, ey - 10, ex, ey + 10, 12.0f);   /* upright, like the buddy */
        }
    }
}

static void set_label(uint16_t bind, char *cache, size_t len, const char *text)
{
    if (strcmp(cache, text) != 0) {
        snprintf(cache, len, "%s", text);
        esp_gsp_set_text(t.ui, bind, text);
    }
}

static void fmt_mmss(char *out, size_t len, int secs)
{
    secs = secs < 0 ? 0 : (secs > APP_ALARM_TIMER_MAX_S ? APP_ALARM_TIMER_MAX_S : secs);
    snprintf(out, len, "%02u:%02u", (unsigned)(secs / 60) % 100u, (unsigned)(secs % 60));
}

static void refresh(void)
{
    app_alarm_timer_t tm;
    const bool have = app_alarm_timer_get(&tm);
    const int64_t now_ms = esp_timer_get_time() / 1000;
    char digits[16], sub[64];
    const char *reset = " ";
    int show_s;
    if (t.dragging) {
        const int mins = (int)fminf(fmaxf(t.acc_deg / 6.0f, 0), 99);
        show_s = mins * 60;
        t.frac = fminf(t.acc_deg, 360.0f) / 360.0f;
        t.knob_deg = fmodf(t.acc_deg, 360.0f);
        t.pulse = 0;
        snprintf(sub, sizeof(sub), "%s", mins ? "release, then tap the centre" : "drag the ring to set");
    } else if (!have) {
        show_s = 0;
        t.frac = 0;
        t.knob_deg = 0;
        t.pulse = 0;
        snprintf(sub, sizeof(sub), "drag the ring to set");
    } else {
        const int rem_s = (tm.remaining_ms + 999) / 1000;
        show_s = tm.done ? 0 : rem_s;
        /* One turn shows up to 60 min; longer timers fill the ring until under an hour. */
        const float total = (float)(tm.total_s < 3600 ? tm.total_s : 3600);
        t.frac = tm.done ? 1.0f : fminf(tm.remaining_ms / 1000.0f / total, 1.0f);
        t.knob_deg = tm.done ? 0 : t.frac * 360.0f;
        if (tm.done) {
            t.pulse = 0.5f + 0.5f * sinf(now_ms / 140.0f);
        } else if (tm.running && rem_s <= 10) {
            t.pulse = 0.5f + 0.5f * sinf(now_ms / 110.0f);
        } else {
            t.pulse = tm.running ? 0.2f : 0;
        }
        char total_txt[16];
        fmt_mmss(total_txt, sizeof(total_txt), tm.total_s);
        if (tm.done) {
            snprintf(sub, sizeof(sub), "done%s%s", tm.label[0] ? " · " : "", tm.label);
            reset = "reset";
        } else if (tm.running) {
            snprintf(sub, sizeof(sub), "of %s%s%s", total_txt, tm.label[0] ? " · " : "", tm.label);
        } else if (tm.remaining_ms >= tm.total_s * 1000) {
            snprintf(sub, sizeof(sub), "tap the centre to start");
            reset = "reset";
        } else {
            snprintf(sub, sizeof(sub), "paused · tap to resume");
            reset = "reset";
        }
    }
    fmt_mmss(digits, sizeof(digits), show_s);
    set_label(t.digits, t.shown_digits, sizeof(t.shown_digits), digits);
    set_label(t.sub, t.shown_sub, sizeof(t.shown_sub), sub);
    set_label(t.reset, t.shown_reset, sizeof(t.shown_reset), reset);
    /* Eyes follow the knob; blink for 140 ms every ~3.7 s. */
    const float ka = (t.knob_deg - 90.0f) * (float)M_PI / 180.0f;
    t.look_x = cosf(ka);
    t.look_y = sinf(ka);
    t.blink = (now_ms % 3700) < 140;
    t.has_timer = have;
}

static void tick(esp_gsp_handle_t ui, void *ctx)
{
    (void)ctx;
    if (t.current != t.scene) {
        return;
    }
    refresh();
    esp_gsp_canvas_invalidate(ui, t.canvas);
}

static void fast_tick(esp_gsp_handle_t ui, void *ctx)
{
    if (t.current != t.scene) {
        return;
    }
    app_alarm_timer_t tm;
    const bool have = app_alarm_timer_get(&tm);
    if (t.dragging || (have && (tm.done || (tm.running && tm.remaining_ms < 11000)))) {
        tick(ui, ctx);
    }
}

bool timer_face_input(int32_t x, int32_t y, bool pressed)
{
    const float dx = x - CX, dy = y - CY, r = sqrtf(dx * dx + dy * dy);
    if (pressed && !t.pressed) {
        t.pressed = true;
        t.px = x;
        t.py = y;
        app_alarm_timer_t tm;
        const bool have = app_alarm_timer_get(&tm);
        if (r > 150 && r < 245 && !(have && tm.running)) {
            /* Start the drag from the current setting so the knob follows the finger. */
            t.dragging = true;
            t.acc_deg = have && !tm.done ? fminf(tm.remaining_ms / 1000.0f / 60.0f, 99) * 6.0f : 0;
            t.last_deg = deg_of((float)x, (float)y);
            if (t.acc_deg < 360.0f) {
                t.acc_deg = t.last_deg;   /* first turn: jump to the finger */
            }
            tick(t.ui, NULL);
            return true;
        }
        return false;
    }
    if (pressed && t.dragging) {
        const float a = deg_of((float)x, (float)y);
        float d = a - t.last_deg;
        if (d > 180) {
            d -= 360;
        } else if (d < -180) {
            d += 360;
        }
        t.last_deg = a;
        t.acc_deg = fminf(fmaxf(t.acc_deg + d, 0), 99 * 6.0f);
        return true;
    }
    if (!pressed && t.pressed) {
        t.pressed = false;
        if (t.dragging) {
            t.dragging = false;
            const int mins = (int)(t.acc_deg / 6.0f + 0.5f);
            if (mins > 0) {
                app_alarm_timer_set(mins * 60, "", false);
            } else {
                app_alarm_timer_reset();
            }
            tick(t.ui, NULL);
            return true;
        }
        const int mx = x - t.px, my = y - t.py;
        if (mx * mx + my * my > 12 * 12) {
            return false;   /* swipe */
        }
        app_alarm_timer_t tm;
        const bool have = app_alarm_timer_get(&tm);
        if (y > 340 && y < 385 && fabsf(dx) < 80 && have && (!tm.running || tm.done)) {
            if (tm.done) {
                app_alarm_dismiss();
            }
            app_alarm_timer_reset();
        } else if (r < 140 && have) {
            if (tm.done) {
                app_alarm_dismiss();
                app_alarm_timer_reset();
            } else {
                app_alarm_timer_toggle();
            }
        }
        tick(t.ui, NULL);
        return false;
    }
    return t.dragging;
}

void timer_face_scene_changed(uint16_t scene_id)
{
    t.current = scene_id;
    if (scene_id == t.scene && t.base) {
        /* Labels and binds are rebuilt with the scene: reset caches, re-register. */
        t.shown_digits[0] = t.shown_sub[0] = t.shown_reset[0] = '\0';
        refresh();
        const esp_gsp_err_t err = esp_gsp_canvas_set_draw_cb(t.ui, t.canvas, draw_cb, NULL);
        if (err != ESP_GSP_OK) {
            ESP_LOGE(TAG, "canvas draw cb: %d", err);
        }
    }
}

bool timer_face_active(void)
{
    return t.current == t.scene && t.base;
}

esp_err_t timer_face_start(esp_gsp_handle_t ui, uint16_t scene, uint16_t canvas, uint16_t digits,
                           uint16_t sub, uint16_t reset)
{
    t.ui = ui;
    t.scene = scene;
    t.canvas = canvas;
    t.digits = digits;
    t.sub = sub;
    t.reset = reset;
    t.base = heap_caps_malloc(FW * FW * 2, MALLOC_CAP_SPIRAM);
    if (!t.base) {
        return ESP_ERR_NO_MEM;
    }
    build_base();
    esp_gsp_timer_create(ui, 1000, tick, NULL);
    esp_gsp_timer_create(ui, 80, fast_tick, NULL);
    ESP_LOGI(TAG, "timer screen ready");
    return ESP_OK;
}
