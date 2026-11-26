// SPDX-License-Identifier: Apache-2.0
/*
 * World-timer clock face (original artwork, see tools/render_clock_assets.py).
 *
 * Layers: the static dial PNG and the RGBA 24 h day/night disc PNG are decoded
 * once into PSRAM. Each minute the disc is rotated to follow UTC and composed
 * over the dial into a cached background; every repaint copies that cache and
 * draws the moving parts with anti-aliased signed-distance shapes: hour and
 * minute swords, the central alarm hand, the chronograph seconds hand and
 * 30-minute counter, the battery ("alarm reserve") hand, the ON/OFF window and
 * the date digits.
 *
 * Input (GSP input interceptor, clock scene only): dragging near the alarm
 * hand sets the daily alarm (consumed, so it doesn't swipe); taps on the ON/OFF
 * window arm it, the top-right / bottom-right corners act as chronograph
 * pushers, and a tap on the city ring makes that city home.
 */
#include "app_clock.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "app_alarm.h"
#include "bsp/battery.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "png.h"

static const char *TAG = "app_clock";

extern const uint8_t dial_png_start[] asm("_binary_worldtimer_dial_png_start");
extern const uint8_t dial_png_end[] asm("_binary_worldtimer_dial_png_end");
extern const uint8_t ring_png_start[] asm("_binary_worldtimer_ring_png_start");
extern const uint8_t ring_png_end[] asm("_binary_worldtimer_ring_png_end");
extern const uint8_t ring_bot_png_start[] asm("_binary_worldtimer_ring_bottom_png_start");
extern const uint8_t ring_bot_png_end[] asm("_binary_worldtimer_ring_bottom_png_end");
extern const uint8_t cities_top_png_start[] asm("_binary_worldtimer_cities_top_png_start");
extern const uint8_t cities_top_png_end[] asm("_binary_worldtimer_cities_top_png_end");
extern const uint8_t cities_bot_png_start[] asm("_binary_worldtimer_cities_bottom_png_start");
extern const uint8_t cities_bot_png_end[] asm("_binary_worldtimer_cities_bottom_png_end");
extern const uint8_t digits_png_start[] asm("_binary_digits_png_start");
extern const uint8_t digits_png_end[] asm("_binary_digits_png_end");

#define FW 480
#define FH 480
#define CX 240.0f
#define CY 240.0f

/* Geometry: must match tools/render_clock_assets.py. */
#define R_RING_OUT 201
#define R_RING_IN 173
#define R_CITY_OUT 232
#define R_CITY_IN 204
#define DATE_X 337
#define DATE_Y 169
#define DIGIT_W 28
#define DIGIT_H 30
#define SUB3_X 320.0f
#define SUB3_Y 252.0f
#define SUB3_R 44.0f
#define SUB7_X 178.0f
#define SUB7_Y 330.0f
#define SUB7_R 44.0f
#define ONOFF_X 163.0f
#define ONOFF_Y 218.0f
#define ONOFF_R 11.0f

/* Colours (RGB888) for the moving parts. */
#define COL_STEEL 0xCDD0D7
#define COL_LUME 0xECEAD6
#define COL_SHADOW 0x081028
#define COL_ALARM 0x2C74E2
#define COL_ALARM_DARK 0x142C68
#define COL_ALARM_TIP 0xE03C2E
#define COL_CHRONO 0xF0F0F4
#define COL_GREEN 0x46C878
#define COL_OFF 0xD64034

static const int8_t CITY_UTC[24] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, -11, -10, -9, -8, -7, -6, -5, -4, -3, -2, -1 };

static struct {
    esp_gsp_handle_t ui;
    uint16_t bind, scene, current_scene;
    uint16_t *dial;        /* RGB565 static dial */
    uint8_t *ring;         /* RGBA 24 h disc */
    uint8_t *cities;       /* RGBA city disc, names reading along the top */
    uint8_t *ring_bot;     /* 24 h disc, numbers reading along the bottom */
    uint8_t *cities_bot;   /* same, names reading along the bottom */
    uint16_t *bg;          /* dial + rotated disc, rebuilt each minute */
    uint8_t *digits;       /* RGB digit strip */
    int bg_minute;         /* UTC minute the cache was built for */
    int home_city;         /* reference city at 12 (index into CITY_UTC) */
    int local_city;        /* the device's own zone (standard time); shows DST-aware local time */
    /* chronograph */
    bool chrono_running;
    int64_t chrono_start_us, chrono_accum_us;
    /* alarm drag */
    bool dragging;
    int drag_minutes;      /* 0..719 on the 12 h dial while dragging */
    int32_t press_x, press_y;
    bool pressed;
    uint8_t battery;       /* 0..100 */
    bool charging;
    int64_t battery_read_us;
} c = { .home_city = 18, .local_city = 18, .bg_minute = -1, .drag_minutes = -1 };

/* ---------- small helpers ---------- */

static inline uint16_t rgb565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

static inline uint16_t blend565(uint16_t dst, uint32_t rgb, float a)
{
    if (a <= 0.0f) {
        return dst;
    }
    if (a >= 1.0f) {
        return rgb565(rgb);
    }
    const int dr = (dst >> 11) << 3, dg = ((dst >> 5) & 0x3F) << 2, db = (dst & 0x1F) << 3;
    const int sr = (rgb >> 16) & 0xFF, sg = (rgb >> 8) & 0xFF, sb = rgb & 0xFF;
    const int r = dr + (int)((sr - dr) * a), g = dg + (int)((sg - dg) * a), b = db + (int)((sb - db) * a);
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static uint8_t *decode_png(const uint8_t *start, const uint8_t *end, uint32_t fmt, int *w, int *h)
{
    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&img, start, (size_t)(end - start))) {
        ESP_LOGE(TAG, "png header: %s", img.message);
        return NULL;
    }
    img.format = fmt;
    uint8_t *buf = heap_caps_malloc(PNG_IMAGE_SIZE(img), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf || !png_image_finish_read(&img, NULL, buf, 0, NULL)) {
        ESP_LOGE(TAG, "png decode: %s", img.message);
        heap_caps_free(buf);
        return NULL;
    }
    *w = (int)img.width;
    *h = (int)img.height;
    return buf;
}

static void local_now(struct tm *out, double *utc_hours)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    const double secs = (double)tv.tv_sec + tv.tv_usec / 1e6;
    *utc_hours = fmod(secs / 3600.0, 24.0);
    if (c.home_city != c.local_city) {
        time_t t = (time_t)(secs + CITY_UTC[c.home_city] * 3600.0);
        gmtime_r(&t, out);
    } else {
        time_t t = (time_t)secs;
        localtime_r(&t, out);
    }
}

/* ---------- background: dial + rotated day/night disc ---------- */

/* Composite an RGBA disc layer, rotated clockwise by deg, over the annulus. */
static void rotate_layer(const uint8_t *src_top, const uint8_t *src_bottom, int r_in, int r_out, float deg)
{
    const float th = deg * (float)M_PI / 180.0f;
    const float cs = cosf(-th), sn = sinf(-th);
    const int r_out2 = r_out * r_out, r_in2 = r_in * r_in;
    for (int y = 240 - r_out; y <= 240 + r_out; ++y) {
        const float dy = y - CY;
        for (int x = 240 - r_out; x <= 240 + r_out; ++x) {
            const float dx = x - CX;
            const int d2 = (int)(dx * dx + dy * dy);
            if (d2 > r_out2 || d2 < r_in2) {
                continue;
            }
            /* Inverse-rotate into the layer image and sample bilinearly. */
            const float sx = CX + dx * cs - dy * sn, sy = CY + dx * sn + dy * cs;
            const int ix = (int)sx, iy = (int)sy;
            if (ix < 0 || iy < 0 || ix >= FW - 1 || iy >= FH - 1) {
                continue;
            }
            const float fx = sx - ix, fy = sy - iy;
            /* Labels sit on 15-degree cells. A label whose cell lands in the
             * top half reads outward, otherwise inward, so text stays upright
             * and no label is split between the two styles. */
            const uint8_t *src = src_top;
            if (src_bottom) {
                const float sa = atan2f(sy - CY, sx - CX) * 180.0f / (float)M_PI + 90.0f;
                float da = roundf(sa / 15.0f) * 15.0f + deg;
                da = fmodf(da, 360.0f);
                if (da < 0) {
                    da += 360.0f;
                }
                src = (da >= 277.5f || da < 82.5f) ? src_top : src_bottom;
            }
            float acc[4] = { 0 };
            const float wts[4] = { (1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy };
            const int idx[4] = { iy * FW + ix, iy * FW + ix + 1, (iy + 1) * FW + ix, (iy + 1) * FW + ix + 1 };
            for (int k = 0; k < 4; ++k) {
                const uint8_t *p = &src[idx[k] * 4];
                for (int ch = 0; ch < 4; ++ch) {
                    acc[ch] += p[ch] * wts[k];
                }
            }
            const uint32_t rgb = ((uint32_t)acc[0] << 16) | ((uint32_t)acc[1] << 8) | (uint32_t)acc[2];
            c.bg[y * FW + x] = blend565(c.bg[y * FW + x], rgb, acc[3] / 255.0f);
        }
    }
}

/* The city disc turns so the reference city sits at 12 (city i is drawn at
 * 90 + 15i degrees), and the 24 h disc turns so the reference city's hour is
 * at 12. Every other city then faces its own hour, and as time passes the
 * 24 h disc creeps anticlockwise, as on a mechanical world-timer. */
static void rebuild_background(double home_hours)
{
    memcpy(c.bg, c.dial, FW * FH * 2);
    rotate_layer(c.cities, c.cities_bot, R_CITY_IN, R_CITY_OUT, -(90.0f + 15.0f * c.home_city));
    rotate_layer(c.ring, c.ring_bot, R_RING_IN, R_RING_OUT, (float)(-15.0 * home_hours));
}

static double tm_hours(const struct tm *lt)
{
    return lt->tm_hour + lt->tm_min / 60.0 + lt->tm_sec / 3600.0;
}

/* Rebuild the cached disc layers when the minute or the reference city changes. */
static void refresh_background(bool force)
{
    struct tm lt;
    double utc_h;
    local_now(&lt, &utc_h);
    const int key = (lt.tm_hour * 60 + lt.tm_min) * 32 + c.home_city;
    if (force || key != c.bg_minute) {
        c.bg_minute = key;
        rebuild_background(tm_hours(&lt));
    }
}

/* ---------- signed-distance shapes, drawn clipped to the paint region ---------- */

typedef struct {
    uint16_t *px;
    int stride;          /* pixels */
    int x0, y0, w, h;    /* region in face coordinates */
} region_t;

static float sd_segment(float px, float py, float ax, float ay, float bx, float by, float r)
{
    const float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
    float h = (pax * bax + pay * bay) / (bax * bax + bay * bay);
    h = h < 0 ? 0 : (h > 1 ? 1 : h);
    const float qx = pax - bax * h, qy = pay - bay * h;
    return sqrtf(qx * qx + qy * qy) - r;
}

/* Convex polygon (counter-clockwise or clockwise), Inigo Quilez's sdPolygon. */
static float sd_poly(float px, float py, const float *v, int n)
{
    float d = (px - v[0]) * (px - v[0]) + (py - v[1]) * (py - v[1]);
    float s = 1.0f;
    for (int i = 0, j = n - 1; i < n; j = i, i++) {
        const float ex = v[j * 2] - v[i * 2], ey = v[j * 2 + 1] - v[i * 2 + 1];
        const float wx = px - v[i * 2], wy = py - v[i * 2 + 1];
        float t = (wx * ex + wy * ey) / (ex * ex + ey * ey);
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        const float bx = wx - ex * t, by = wy - ey * t;
        const float dd = bx * bx + by * by;
        if (dd < d) {
            d = dd;
        }
        const bool c1 = py >= v[i * 2 + 1], c2 = py < v[j * 2 + 1], c3 = ex * wy > ey * wx;
        if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) {
            s = -s;
        }
    }
    return s * sqrtf(d);
}

typedef enum { SH_SEG, SH_POLY, SH_CIRCLE, SH_RING } shape_kind_t;   /* ring: x y r half-width */
typedef struct {
    shape_kind_t kind;
    float a[12];   /* seg: ax ay bx by r | poly: up to 6 points | circle: x y r */
    int n;
    uint32_t rgb;
    float alpha;
} shape_t;

static void shape_bbox(const shape_t *s, int *x0, int *y0, int *x1, int *y1)
{
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f, pad = 2;
    if (s->kind == SH_SEG) {
        minx = fminf(s->a[0], s->a[2]); maxx = fmaxf(s->a[0], s->a[2]);
        miny = fminf(s->a[1], s->a[3]); maxy = fmaxf(s->a[1], s->a[3]);
        pad += s->a[4];
    } else if (s->kind == SH_CIRCLE || s->kind == SH_RING) {
        const float rr = s->a[2] + (s->kind == SH_RING ? s->a[3] : 0);
        minx = s->a[0] - rr; maxx = s->a[0] + rr; miny = s->a[1] - rr; maxy = s->a[1] + rr;
    } else {
        for (int i = 0; i < s->n; ++i) {
            minx = fminf(minx, s->a[i * 2]); maxx = fmaxf(maxx, s->a[i * 2]);
            miny = fminf(miny, s->a[i * 2 + 1]); maxy = fmaxf(maxy, s->a[i * 2 + 1]);
        }
    }
    *x0 = (int)floorf(minx - pad); *y0 = (int)floorf(miny - pad);
    *x1 = (int)ceilf(maxx + pad); *y1 = (int)ceilf(maxy + pad);
}

static void draw_shape(const region_t *r, const shape_t *s)
{
    int x0, y0, x1, y1;
    shape_bbox(s, &x0, &y0, &x1, &y1);
    if (x0 < r->x0) x0 = r->x0;
    if (y0 < r->y0) y0 = r->y0;
    if (x1 > r->x0 + r->w - 1) x1 = r->x0 + r->w - 1;
    if (y1 > r->y0 + r->h - 1) y1 = r->y0 + r->h - 1;
    for (int y = y0; y <= y1; ++y) {
        uint16_t *row = r->px + (y - r->y0) * r->stride - r->x0;
        for (int x = x0; x <= x1; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            float d;
            if (s->kind == SH_SEG) {
                d = sd_segment(px, py, s->a[0], s->a[1], s->a[2], s->a[3], s->a[4]);
            } else if (s->kind == SH_CIRCLE) {
                d = sqrtf((px - s->a[0]) * (px - s->a[0]) + (py - s->a[1]) * (py - s->a[1])) - s->a[2];
            } else if (s->kind == SH_RING) {
                d = fabsf(sqrtf((px - s->a[0]) * (px - s->a[0]) + (py - s->a[1]) * (py - s->a[1])) - s->a[2]) - s->a[3];
            } else {
                d = sd_poly(px, py, s->a, s->n);
            }
            float a = 0.5f - d;
            if (a <= 0) {
                continue;
            }
            row[x] = blend565(row[x], s->rgb, (a > 1 ? 1 : a) * s->alpha);
        }
    }
}

static void pol(float r, float deg, float cx, float cy, float *x, float *y)
{
    const float a = (deg - 90.0f) * (float)M_PI / 180.0f;
    *x = cx + r * cosf(a);
    *y = cy + r * sinf(a);
}

/* Sword hand: tapered blade with a lume stripe and a soft shadow. */
static void draw_sword(const region_t *r, float deg, float len, float width)
{
    const float a = (deg - 90.0f) * (float)M_PI / 180.0f;
    const float ux = cosf(a), uy = sinf(a), px = -uy, py = ux;
    float tipx, tipy, midx, midy;
    pol(len, deg, CX, CY, &tipx, &tipy);
    pol(len * 0.55f, deg, CX, CY, &midx, &midy);
    const float bx = CX - ux * 14, by = CY - uy * 14;
    const float v[10] = { bx + px * width * 0.42f, by + py * width * 0.42f, midx + px * width * 0.5f, midy + py * width * 0.5f,
                          tipx, tipy, midx - px * width * 0.5f, midy - py * width * 0.5f,
                          bx - px * width * 0.42f, by - py * width * 0.42f };
    shape_t sh = { .kind = SH_POLY, .n = 5, .rgb = COL_SHADOW, .alpha = 0.55f };
    for (int i = 0; i < 5; ++i) {
        sh.a[i * 2] = v[i * 2] + 1.3f;
        sh.a[i * 2 + 1] = v[i * 2 + 1] + 1.6f;
    }
    draw_shape(r, &sh);
    shape_t blade = { .kind = SH_POLY, .n = 5, .rgb = COL_STEEL, .alpha = 1 };
    memcpy(blade.a, v, sizeof(v));
    draw_shape(r, &blade);
    float l0x, l0y, l1x, l1y;
    pol(len * 0.25f, deg, CX, CY, &l0x, &l0y);
    pol(len * 0.8f, deg, CX, CY, &l1x, &l1y);
    shape_t lume = { .kind = SH_SEG, .a = { l0x, l0y, l1x, l1y, width * 0.17f }, .rgb = COL_LUME, .alpha = 1 };
    draw_shape(r, &lume);
}

/* Central alarm hand: a skeletonized blue frame (open lozenge) with a solid
 * red arrow tip and a hollow ring counterweight. */
static void draw_alarm_hand(const region_t *r, float deg, bool armed)
{
    const float a = (deg - 90.0f) * (float)M_PI / 180.0f;
    const float ux = cosf(a), uy = sinf(a), px = -uy, py = ux;
    const float alpha = armed ? 1.0f : 0.6f;
    /* Frame outline: tail (r=-22) -> widest (r=58, +-6.5) -> neck (r=132, +-2) */
    const float pts[4][2] = { { -22, 0 }, { 58, 6.5f }, { 132, 2.0f }, { 132, -2.0f } };
    float x[5], y[5];
    x[0] = CX + ux * pts[0][0]; y[0] = CY + uy * pts[0][0];
    x[1] = CX + ux * pts[1][0] + px * pts[1][1]; y[1] = CY + uy * pts[1][0] + py * pts[1][1];
    x[2] = CX + ux * pts[2][0] + px * pts[2][1]; y[2] = CY + uy * pts[2][0] + py * pts[2][1];
    x[3] = CX + ux * pts[1][0] - px * pts[1][1]; y[3] = CY + uy * pts[1][0] - py * pts[1][1];
    x[4] = CX + ux * pts[2][0] - px * pts[2][1]; y[4] = CY + uy * pts[2][0] - py * pts[2][1];
    const int edges[4][2] = { { 0, 1 }, { 1, 2 }, { 0, 3 }, { 3, 4 } };
    for (int i = 0; i < 4; ++i) {
        shape_t e = { .kind = SH_SEG, .a = { x[edges[i][0]], y[edges[i][0]], x[edges[i][1]], y[edges[i][1]], 1.3f },
                      .rgb = COL_ALARM, .alpha = alpha };
        draw_shape(r, &e);
    }
    /* Solid red arrow tip */
    float tx, ty, bx, by;
    pol(160, deg, CX, CY, &tx, &ty);
    pol(128, deg, CX, CY, &bx, &by);
    shape_t head = { .kind = SH_POLY, .n = 3, .a = { tx, ty, bx + px * 7.5f, by + py * 7.5f, bx - px * 7.5f, by - py * 7.5f },
                     .rgb = COL_ALARM_TIP, .alpha = alpha };
    draw_shape(r, &head);
    /* Hollow counterweight */
    float cx, cy;
    pol(-32, deg, CX, CY, &cx, &cy);
    shape_t cw = { .kind = SH_RING, .a = { cx, cy, 7.5f, 1.6f }, .rgb = COL_ALARM, .alpha = alpha };
    draw_shape(r, &cw);
}

static void draw_small_hand(const region_t *r, float cx, float cy, float len, float deg, float w, uint32_t rgb)
{
    float ex, ey;
    pol(len, deg, cx, cy, &ex, &ey);
    shape_t s = { .kind = SH_SEG, .a = { cx, cy, ex, ey, w }, .rgb = rgb, .alpha = 1 };
    draw_shape(r, &s);
    shape_t hub = { .kind = SH_CIRCLE, .a = { cx, cy, w + 2.5f }, .rgb = rgb, .alpha = 1 };
    draw_shape(r, &hub);
}

static void blit_digit(const region_t *r, int digit, int dx, int dy)
{
    for (int y = 0; y < DIGIT_H; ++y) {
        const int fy = dy + y;
        if (fy < r->y0 || fy >= r->y0 + r->h) {
            continue;
        }
        for (int x = 0; x < DIGIT_W; ++x) {
            const int fx = dx + x;
            if (fx < r->x0 || fx >= r->x0 + r->w) {
                continue;
            }
            const uint8_t *p = &c.digits[(y * DIGIT_W * 10 + digit * DIGIT_W + x) * 3];
            r->px[(fy - r->y0) * r->stride + (fx - r->x0)] = rgb565(((uint32_t)p[0] << 16) | (p[1] << 8) | p[2]);
        }
    }
}

/* ---------- state helpers ---------- */

static int64_t chrono_elapsed_us(void)
{
    return c.chrono_accum_us + (c.chrono_running ? esp_timer_get_time() - c.chrono_start_us : 0);
}

uint32_t app_clock_chrono_seconds(void)
{
    return (uint32_t)(chrono_elapsed_us() / 1000000);
}

static void read_battery(void)
{
    const int64_t now = esp_timer_get_time();
    if (c.battery_read_us && now - c.battery_read_us < 30LL * 1000 * 1000) {
        return;
    }
    c.battery_read_us = now;
    bsp_battery_status_t st;
    if (bsp_battery_read(&st) == ESP_OK) {
        c.battery = st.state_of_charge > 100 ? 100 : st.state_of_charge;
        c.charging = st.current_ma > 20;
    }
}

/* ---------- canvas draw ---------- */

static void draw_face(const esp_gsp_canvas_surface_t *surface, void *ctx)
{
    (void)ctx;
    region_t r = { .px = surface->pixels, .stride = (int)(surface->stride_bytes / 2),
                   .x0 = surface->x, .y0 = surface->y, .w = surface->width, .h = surface->height };
    if (surface->pixel_format != ESP_GSP_CANVAS_PIXEL_RGB565) {
        for (int y = 0; y < r.h; ++y) {
            memset((uint8_t *)surface->pixels + y * surface->stride_bytes, 0, (size_t)r.w * 3);
        }
        return;
    }
    for (int y = 0; y < r.h; ++y) {
        memcpy(r.px + y * r.stride, c.bg + (r.y0 + y) * FW + r.x0, (size_t)r.w * 2);
    }
    struct tm lt;
    double utc_h;
    local_now(&lt, &utc_h);

    /* Date wheel */
    blit_digit(&r, lt.tm_mday / 10, DATE_X - 29, DATE_Y - DIGIT_H / 2);
    blit_digit(&r, lt.tm_mday % 10, DATE_X + 1, DATE_Y - DIGIT_H / 2);

    /* Alarm ON/OFF window (green half when armed) */
    int ah = -1, am = 0;
    bool armed = false;
    const bool has_alarm = app_alarm_get_watch(&ah, &am, &armed);
    shape_t win = { .kind = SH_CIRCLE, .a = { ONOFF_X, ONOFF_Y, ONOFF_R }, .rgb = armed ? COL_GREEN : COL_OFF, .alpha = 1 };
    draw_shape(&r, &win);

    /* Battery as the alarm reserve: the dial arc runs 240 (empty) .. 340 (full)
     * degrees clockwise from 12. */
    const float bdeg = 240.0f + c.battery;
    draw_small_hand(&r, SUB7_X, SUB7_Y, SUB7_R - 12, bdeg, 1.4f, c.charging ? COL_GREEN : 0xEBEBF0);

    /* 30-minute counter */
    const double chrono_s = (double)chrono_elapsed_us() / 1e6;
    draw_small_hand(&r, SUB3_X, SUB3_Y, SUB3_R - 10, (float)(fmod(chrono_s / 60.0, 30.0) * 12.0), 1.8f, COL_LUME);

    /* Alarm hand (12 h dial), under the time hands */
    int alarm_min = c.dragging ? c.drag_minutes : (has_alarm ? (ah % 12) * 60 + am : -1);
    if (alarm_min >= 0) {
        draw_alarm_hand(&r, alarm_min * 0.5f, armed || c.dragging);
    }

    /* Hour and minute */
    const float minute = lt.tm_min + lt.tm_sec / 60.0f;
    draw_sword(&r, (lt.tm_hour % 12) * 30.0f + minute * 0.5f, 114, 22);
    draw_sword(&r, minute * 6.0f, 174, 18);

    /* Chronograph seconds: sweeps while running, rests at 12 otherwise */
    const float cdeg = (float)(fmod(chrono_s, 60.0) * 6.0);
    float ex, ey, tx, ty;
    pol(196, cdeg, CX, CY, &ex, &ey);
    pol(-34, cdeg, CX, CY, &tx, &ty);
    shape_t ch = { .kind = SH_SEG, .a = { tx, ty, ex, ey, 1.2f }, .rgb = COL_CHRONO, .alpha = 1 };
    draw_shape(&r, &ch);

    shape_t hub = { .kind = SH_CIRCLE, .a = { CX, CY, 12 }, .rgb = 0xC8CBD2, .alpha = 1 };
    draw_shape(&r, &hub);
    shape_t hub2 = { .kind = SH_CIRCLE, .a = { CX, CY, 5 }, .rgb = COL_ALARM, .alpha = 1 };
    draw_shape(&r, &hub2);
}

/* ---------- ticking and input ---------- */

static void tick(esp_gsp_handle_t ui, void *ctx)
{
    (void)ctx;
    if (c.current_scene != c.scene) {
        return;
    }
    refresh_background(false);
    read_battery();
    esp_gsp_canvas_invalidate(ui, c.bind);
}

static float angle_of(int32_t x, int32_t y)
{
    float deg = atan2f((float)y - CY, (float)x - CX) * 180.0f / (float)M_PI + 90.0f;
    return deg < 0 ? deg + 360.0f : deg;
}

static bool on_input(esp_gsp_handle_t ui, int32_t x, int32_t y, bool pressed, void *ctx)
{
    (void)ui;
    (void)ctx;
    if (c.current_scene != c.scene) {
        return false;
    }
    const float dx = x - CX, dy = y - CY, rad = sqrtf(dx * dx + dy * dy);
    if (pressed && !c.pressed) {
        c.pressed = true;
        c.press_x = x;
        c.press_y = y;
        /* Grab the alarm hand near its arrowhead (or anywhere along it). */
        int ah = -1, am = 0;
        bool on = false;
        if (app_alarm_get_watch(&ah, &am, &on) || true) {
            const float hand_deg = ah >= 0 ? ((ah % 12) * 60 + am) * 0.5f : 0.0f;
            float diff = fabsf(angle_of(x, y) - hand_deg);
            if (diff > 180) {
                diff = 360 - diff;
            }
            if (rad > 60 && rad < 172 && diff < 14) {
                c.dragging = true;
                c.drag_minutes = ah >= 0 ? (ah % 12) * 60 + am : 0;
                return true;
            }
        }
        return false;
    }
    if (pressed && c.dragging) {
        c.drag_minutes = ((int)(angle_of(x, y) * 2.0f) / 5 * 5) % 720;   /* 5-minute steps */
        esp_gsp_canvas_invalidate(c.ui, c.bind);
        return true;
    }
    if (!pressed && c.pressed) {
        c.pressed = false;
        if (c.dragging) {
            c.dragging = false;
            /* 12 h dial: choose the next occurrence (am or pm) after now. */
            struct tm lt;
            double utc_h;
            local_now(&lt, &utc_h);
            const int now_m = lt.tm_hour * 60 + lt.tm_min;
            int m = c.drag_minutes;
            if (m <= now_m) {
                m += 720;
            }
            if (m <= now_m) {
                m += 720;
            }
            m %= 1440;
            app_alarm_set_watch(m / 60, m % 60, true);
            esp_gsp_canvas_invalidate(c.ui, c.bind);
            return true;
        }
        const int mx = x - c.press_x, my = y - c.press_y;
        if (mx * mx + my * my > 12 * 12) {
            return false;   /* a swipe, not a tap */
        }
        const float odx = x - ONOFF_X, ody = y - ONOFF_Y;
        if (odx * odx + ody * ody < 24 * 24 || (x < 70 && y > 410)) {   /* window, or the 8 o'clock alarm pusher */
            int ah, am;
            bool on;
            if (app_alarm_get_watch(&ah, &am, &on)) {
                app_alarm_set_watch(ah, am, !on);
            } else {
                app_alarm_set_watch(7, 0, true);
            }
        } else if (x > 410 && y < 70) {                  /* top-right pusher: start/stop */
            if (c.chrono_running) {
                c.chrono_accum_us += esp_timer_get_time() - c.chrono_start_us;
                c.chrono_running = false;
            } else {
                c.chrono_start_us = esp_timer_get_time();
                c.chrono_running = true;
            }
        } else if (x > 410 && y > 410) {                 /* bottom-right pusher: reset */
            if (!c.chrono_running) {
                c.chrono_accum_us = 0;
            }
        } else if (x < 70 && y < 70) {                   /* 10 o'clock world-time crown: one zone east */
            c.home_city = (c.home_city + 1) % 24;
            refresh_background(true);
            ESP_LOGI(TAG, "reference city %d (UTC%+d)", c.home_city, CITY_UTC[c.home_city]);
        } else if (rad >= R_CITY_IN && rad <= R_CITY_OUT + 6) {
            /* Tap a city to bring it to 12; tap the one at 12 to return home. */
            int i = c.home_city + (int)lroundf(angle_of(x, y) / 15.0f);
            i = ((i % 24) + 24) % 24;
            c.home_city = (i == c.home_city) ? c.local_city : i;
            refresh_background(true);
            ESP_LOGI(TAG, "reference city %d (UTC%+d)", c.home_city, CITY_UTC[c.home_city]);
        }
        esp_gsp_canvas_invalidate(c.ui, c.bind);
        return false;
    }
    return false;
}

void app_clock_scene_changed(uint16_t scene_id)
{
    c.current_scene = scene_id;
    if (scene_id == c.scene && c.ui && c.bg) {
        /* Binds are scene-scoped and the scene is rebuilt on arrival, so the
         * draw callback is (re)registered each time the clock is shown. */
        refresh_background(true);
        const esp_gsp_err_t err = esp_gsp_canvas_set_draw_cb(c.ui, c.bind, draw_face, NULL);
        if (err != ESP_GSP_OK) {
            ESP_LOGE(TAG, "canvas draw cb: %d", err);
        }
    }
}

/* Repaint fast while the chronograph sweeps, otherwise once a second. */
static void fast_tick(esp_gsp_handle_t ui, void *ctx)
{
    if (c.chrono_running || c.dragging) {
        tick(ui, ctx);
    }
}

esp_err_t app_clock_start(esp_gsp_handle_t ui, uint16_t canvas_bind, uint16_t scene_id)
{
    c.ui = ui;
    c.bind = canvas_bind;
    c.scene = scene_id;
    int w, h, rw, rh, dw, dh;
    uint8_t *dial_rgb = decode_png(dial_png_start, dial_png_end, PNG_FORMAT_RGB, &w, &h);
    c.ring = decode_png(ring_png_start, ring_png_end, PNG_FORMAT_RGBA, &rw, &rh);
    int cw = 0, ch = 0;
    int bw = 0, bh = 0;
    c.cities = decode_png(cities_top_png_start, cities_top_png_end, PNG_FORMAT_RGBA, &cw, &ch);
    int qw = 0, qh = 0;
    c.ring_bot = decode_png(ring_bot_png_start, ring_bot_png_end, PNG_FORMAT_RGBA, &qw, &qh);
    c.cities_bot = decode_png(cities_bot_png_start, cities_bot_png_end, PNG_FORMAT_RGBA, &bw, &bh);
    c.digits = decode_png(digits_png_start, digits_png_end, PNG_FORMAT_RGB, &dw, &dh);
    c.dial = heap_caps_malloc(FW * FH * 2, MALLOC_CAP_SPIRAM);
    c.bg = heap_caps_malloc(FW * FH * 2, MALLOC_CAP_SPIRAM);
    if (!dial_rgb || !c.ring || !c.cities || cw != FW || ch != FH || !c.ring_bot || qw != FW || qh != FH || !c.cities_bot || bw != FW || bh != FH || !c.digits || !c.dial || !c.bg || w != FW || h != FH || rw != FW || rh != FH ||
            dw != DIGIT_W * 10 || dh != DIGIT_H) {
        ESP_LOGE(TAG, "clock assets unavailable");
        heap_caps_free(dial_rgb);
        return ESP_FAIL;
    }
    for (int i = 0; i < FW * FH; ++i) {
        c.dial[i] = rgb565(((uint32_t)dial_rgb[i * 3] << 16) | (dial_rgb[i * 3 + 1] << 8) | dial_rgb[i * 3 + 2]);
    }
    heap_caps_free(dial_rgb);
    memcpy(c.bg, c.dial, FW * FH * 2);
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "battery gauge unavailable; alarm reserve reads empty");
    }
    read_battery();
    {
        /* Home city = the device zone's standard-time offset (TZ from Kconfig). */
        const time_t jan = (time_t)20468 * 86400 + 12 * 3600;   /* 2026-01-15 12:00 UTC */
        struct tm lt;
        localtime_r(&jan, &lt);
        int off = lt.tm_hour - 12 + (lt.tm_mday - 15) * 24;
        off = ((off % 24) + 24) % 24;            /* index i has UTC offset i (or i-24) */
        c.local_city = c.home_city = off;
        ESP_LOGI(TAG, "home city %d (UTC%+d)", off, CITY_UTC[off]);
    }
    esp_gsp_set_input_interceptor(ui, on_input, NULL);
    esp_gsp_timer_create(ui, 1000, tick, NULL);
    esp_gsp_timer_create(ui, 100, fast_tick, NULL);
    ESP_LOGI(TAG, "world-timer face ready");
    return ESP_OK;
}
