// SPDX-License-Identifier: Apache-2.0
/*
 * Workout screen: big digits readable across a garage, the whole face tinted
 * by phase (amber get-ready, green work, blue rest, gold done) and a thick
 * ring that drains through the current phase. The engine is app_workout; this
 * file only draws and handles touch.
 *
 * Touch: tap the title to change preset (when idle), the centre to start /
 * pause / resume, the left or right of the rounds line for -/+ rounds (idle),
 * "reset" at the bottom when paused or done.
 */
#include "clock_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_workout.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "workout_face";

#define FW 480
#define CX 240.0f
#define CY 240.0f
#define R_RING 216.0f
#define CORE 13.0f

typedef struct { int r, g, b; } rgb_t;

static struct {
    esp_gsp_handle_t ui;
    workout_face_ids_t id;
    uint16_t current;
    uint16_t *base;
    rgb_t base_col;
    bool base_valid;
    bool pressed;
    int32_t px, py;
    char sh_title[40], sh_phase[24], sh_digits[12], sh_round[40], sh_btn[24];
    /* frame snapshot for the draw callback */
    rgb_t col;
    float frac;
    float pulse;
    float drawn_frac;
    int64_t drawn_us;
    bool dirty;
} f;

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

static float deg_of(float x, float y)
{
    float d = atan2f(y - CY, x - CX) * 180.0f / (float)M_PI + 90.0f;
    return d < 0 ? d + 360.0f : d;
}

static rgb_t phase_col(const app_workout_state_t *st)
{
    switch (st->phase) {
    case APP_WORKOUT_PREP: return (rgb_t){ 255, 170, 30 };
    case APP_WORKOUT_WORK: return (rgb_t){ 40, 215, 100 };
    case APP_WORKOUT_REST: return (rgb_t){ 70, 150, 255 };
    case APP_WORKOUT_DONE: return (rgb_t){ 255, 200, 60 };
    default:               return (rgb_t){ 110, 120, 140 };
    }
}

/* Tinted disc plus a dark track under the ring, in the phase colour. */
static void build_base(rgb_t c)
{
    for (int y = 0; y < FW; ++y) {
        for (int x = 0; x < FW; ++x) {
            const float dx = x + 0.5f - CX, dy = y + 0.5f - CY;
            const float r = sqrtf(dx * dx + dy * dy);
            uint16_t p = 0;
            if (r < R_RING - CORE) {
                /* brighter toward the middle so the digits sit on a glow */
                const float k = 0.10f + 0.12f * (1.0f - r / (R_RING - CORE));
                p = rgb565((int)(c.r * k), (int)(c.g * k), (int)(c.b * k));
                const float edge = (R_RING - CORE) - r;
                if (edge < 1.0f) {
                    p = mix(0, (int)(c.r * k), (int)(c.g * k), (int)(c.b * k), edge);
                }
            }
            const float d = fabsf(r - R_RING) - CORE;
            if (d < 0.5f) {
                p = mix(p, c.r / 5, c.g / 5, c.b / 5, 0.5f - d);
            }
            f.base[y * FW + x] = p;
        }
    }
    f.base_col = c;
    f.base_valid = true;
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
        memcpy(px + y * stride, f.base + (y0 + y) * FW + x0, (size_t)w * 2);
    }
    const float span = f.frac * 360.0f;
    if (span <= 0) {
        return;
    }
    const float gain = 0.75f + 0.25f * f.pulse;
    const int cr = (int)(f.col.r * gain), cg = (int)(f.col.g * gain), cb = (int)(f.col.b * gain);
    for (int y = y0; y < y0 + h; ++y) {
        const float dy = y + 0.5f - CY;
        if (fabsf(dy) > R_RING + CORE + 1) {
            continue;
        }
        uint16_t *row = px + (y - y0) * stride - x0;
        /* skip the empty middle of the ring on this row */
        const float ri = R_RING - CORE - 1;
        const float hole = fabsf(dy) < ri ? sqrtf(ri * ri - dy * dy) : 0;
        for (int x = x0; x < x0 + w; ++x) {
            const float dx = x + 0.5f - CX;
            if (fabsf(dx) < hole) {
                x = (int)(CX + hole) - 1;
                continue;
            }
            const float r = sqrtf(dx * dx + dy * dy);
            const float d = fabsf(r - R_RING);
            if (d > CORE + 1) {
                continue;
            }
            const float a = deg_of(x + 0.5f, y + 0.5f);
            float off = 0;
            if (a > span && span < 359.5f) {
                off = fminf(a - span, 360.0f - a) * R_RING * (float)M_PI / 180.0f;
            }
            const float dist = sqrtf(d * d + off * off);
            if (dist < CORE + 0.5f) {
                row[x] = mix(row[x], cr, cg, cb, CORE + 0.5f - dist);
            }
        }
    }
}

static void set_label(uint16_t bind, char *cache, size_t len, const char *text)
{
    if (strcmp(cache, text) != 0) {
        snprintf(cache, len, "%s", text);
        esp_gsp_set_text(f.ui, bind, text);
    }
}

static void fmt_mmss(char *out, size_t len, int secs)
{
    secs = secs < 0 ? 0 : (secs > 99 * 60 + 59 ? 99 * 60 + 59 : secs);
    snprintf(out, len, "%02d:%02d", secs / 60, secs % 60);
}

static void fmt_short(char *out, size_t len, int secs)
{
    if (secs % 60 == 0 && secs >= 60) {
        snprintf(out, len, "%d:00", secs / 60);
    } else if (secs >= 60) {
        snprintf(out, len, "%d:%02d", secs / 60, secs % 60);
    } else {
        snprintf(out, len, "%d", secs);
    }
}

static void refresh(void)
{
    app_workout_state_t st;
    app_workout_get(&st);
    const int64_t now_ms = esp_timer_get_time() / 1000;
    char title[40], phase[24], digits[12], round[40], btn[24] = " ";
    const app_workout_plan_t *p = &st.plan;
    char ws[12], rs[12];
    fmt_short(ws, sizeof(ws), p->work_s);
    fmt_short(rs, sizeof(rs), p->rest_s);
    const bool emom = st.preset == APP_WORKOUT_EMOM;
    const bool watch = st.total_s == 0;
    if (watch) {
        snprintf(title, sizeof(title), "%s", st.name);
    } else if (emom || p->rest_s == 0) {
        snprintf(title, sizeof(title), "%s %s", st.name, ws);
    } else {
        snprintf(title, sizeof(title), "%s %s/%s", st.name, ws, rs);
    }
    const char *unit = emom ? "MINUTE" : "ROUND";
    f.pulse = 0;
    switch (st.phase) {
    case APP_WORKOUT_IDLE:
        snprintf(phase, sizeof(phase), "TAP TO START");
        fmt_mmss(digits, sizeof(digits), watch ? 0 : st.total_s);
        if (watch) {
            snprintf(round, sizeof(round), "counts up");
        } else {
            snprintf(round, sizeof(round), "-   %d %s%s   +", p->rounds, emom ? "MINUTE" : "ROUND",
                     p->rounds == 1 ? "" : "S");
        }
        snprintf(btn, sizeof(btn), "tap title: next");
        f.frac = 1.0f;
        break;
    case APP_WORKOUT_PREP:
        snprintf(phase, sizeof(phase), st.paused ? "PAUSED" : "GET READY");
        fmt_mmss(digits, sizeof(digits), (st.phase_left_ms + 999) / 1000);
        snprintf(round, sizeof(round), watch ? " " : "%d %s%s", p->rounds, unit, p->rounds == 1 ? "" : "S");
        f.frac = st.phase_left_ms / (APP_WORKOUT_PREP_S * 1000.0f);
        break;
    case APP_WORKOUT_WORK:
    case APP_WORKOUT_REST:
        if (st.paused) {
            snprintf(phase, sizeof(phase), "PAUSED");
            snprintf(btn, sizeof(btn), "reset");
        } else {
            snprintf(phase, sizeof(phase), watch ? "GO" : (st.phase == APP_WORKOUT_WORK ? (emom ? "GO" : "WORK") : "REST"));
        }
        if (watch) {
            fmt_mmss(digits, sizeof(digits), st.phase_left_ms / 1000);
            snprintf(round, sizeof(round), " ");
            f.frac = (st.phase_left_ms % 60000) / 60000.0f;
            if (f.frac < 0.002f) {
                f.frac = 0.002f;
            }
        } else {
            const int left = (st.phase_left_ms + 999) / 1000;
            fmt_mmss(digits, sizeof(digits), left);
            snprintf(round, sizeof(round), "%s %d / %d", unit, st.round, p->rounds);
            f.frac = st.phase_len_s ? st.phase_left_ms / (st.phase_len_s * 1000.0f) : 0;
            if (left <= 3 && !st.paused) {
                f.pulse = 0.5f + 0.5f * sinf(now_ms / 90.0f);
            }
        }
        break;
    case APP_WORKOUT_DONE:
    default:
        snprintf(phase, sizeof(phase), "DONE");
        fmt_mmss(digits, sizeof(digits), st.total_s);
        snprintf(round, sizeof(round), "%d %s%s", p->rounds, unit, p->rounds == 1 ? "" : "S");
        snprintf(btn, sizeof(btn), "reset");
        f.frac = 1.0f;
        f.pulse = 0.5f + 0.5f * sinf(now_ms / 300.0f);
        break;
    }
    rgb_t c = phase_col(&st);
    if (st.paused) {
        c = (rgb_t){ c.r / 2, c.g / 2, c.b / 2 };
    }
    if (!f.base_valid || c.r != f.base_col.r || c.g != f.base_col.g || c.b != f.base_col.b) {
        build_base(c);
        f.dirty = true;
    }
    f.col = c;
    set_label(f.id.title, f.sh_title, sizeof(f.sh_title), title);
    set_label(f.id.phase, f.sh_phase, sizeof(f.sh_phase), phase);
    set_label(f.id.digits, f.sh_digits, sizeof(f.sh_digits), digits);
    set_label(f.id.round, f.sh_round, sizeof(f.sh_round), round);
    set_label(f.id.btn, f.sh_btn, sizeof(f.sh_btn), btn);
}

static void tick(esp_gsp_handle_t ui, void *ctx)
{
    (void)ctx;
    if (f.current != f.id.scene || !f.base) {
        return;
    }
    refresh();
    /* Repaint the canvas at most 5x a second, and only when something moved. */
    const int64_t now = esp_timer_get_time();
    const bool moved = fabsf(f.frac - f.drawn_frac) > 0.002f || f.pulse > 0 || f.dirty;
    if (moved && now - f.drawn_us >= 190000) {
        f.drawn_frac = f.frac;
        f.drawn_us = now;
        f.dirty = false;
        esp_gsp_canvas_invalidate(ui, f.id.canvas);
    }
}

bool workout_face_active(void)
{
    return f.current == f.id.scene && f.base;
}

bool workout_face_input(int32_t x, int32_t y, bool pressed)
{
    if (pressed && !f.pressed) {
        f.pressed = true;
        f.px = x;
        f.py = y;
        return false;
    }
    if (pressed || !f.pressed) {
        return false;
    }
    f.pressed = false;
    const int mx = x - f.px, my = y - f.py;
    if (mx * mx + my * my > 14 * 14) {
        return false;   /* swipe */
    }
    app_workout_state_t st;
    app_workout_get(&st);
    const bool idle = st.phase == APP_WORKOUT_IDLE || st.phase == APP_WORKOUT_DONE;
    const float dx = x - CX;
    if (y < 158) {
        if (idle) {
            app_workout_next_preset();
        }
    } else if (y >= 300 && y < 350 && idle && st.total_s > 0 && fabsf(dx) > 40) {
        app_workout_adjust_rounds(dx < 0 ? -1 : 1);
    } else if (y >= 350 && y < 400 && fabsf(dx) < 100 && (st.paused || st.phase == APP_WORKOUT_DONE)) {
        app_workout_stop();
    } else if (y >= 158 && y < 350) {
        app_workout_toggle();
    }
    tick(f.ui, NULL);
    return false;
}

void workout_face_scene_changed(uint16_t scene_id)
{
    f.current = scene_id;
    if (scene_id == f.id.scene && f.base) {
        f.sh_title[0] = f.sh_phase[0] = f.sh_digits[0] = f.sh_round[0] = f.sh_btn[0] = '\0';
        refresh();
        f.dirty = true;
        const esp_gsp_err_t err = esp_gsp_canvas_set_draw_cb(f.ui, f.id.canvas, draw_cb, NULL);
        if (err != ESP_GSP_OK) {
            ESP_LOGE(TAG, "canvas draw cb: %d", err);
        }
    }
}

esp_err_t workout_face_start(esp_gsp_handle_t ui, const workout_face_ids_t *ids)
{
    f.ui = ui;
    f.id = *ids;
    f.current = 0xFFFF;
    f.base = heap_caps_malloc(FW * FW * 2, MALLOC_CAP_SPIRAM);
    if (!f.base) {
        return ESP_ERR_NO_MEM;
    }
    esp_gsp_timer_create(ui, 100, tick, NULL);
    ESP_LOGI(TAG, "workout screen ready");
    return ESP_OK;
}
