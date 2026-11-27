// SPDX-License-Identifier: Apache-2.0
/*
 * Calendar screen: the current or next event large (countdown, title, time and
 * place, a progress bar), then the four after it, and today's all-day events.
 * Also feeds a one-line "Next: 2:30 PM Standup" note to the eyes screen.
 *
 * Data comes from the relay's GET /calendar (private iCal feeds, recurrences
 * expanded there), fetched on the voice relay task like the weather. Refreshes
 * every 5 minutes whether or not the screen is showing, since the eyes note
 * needs it; the countdowns tick from the cached copy.
 */
#include "clock_internal.h"

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

static const char *TAG = "calendar_face";

#define FW 480
#define JSON_MAX 6144
#define MAX_EV 24
#define ROWS 4
#define ROW_Y0 256
#define ROW_DY 42
#define BAR_X0 120
#define BAR_X1 360
#define BAR_Y 228
#define REFRESH_US (5LL * 60 * 1000 * 1000)
#define BOOT_FETCH_US (25LL * 1000 * 1000)
#define RETRY_US (60LL * 1000 * 1000)
#define NOTE_AHEAD_S (3 * 3600)   /* eyes note: events starting within 3 hours */

typedef struct {
    time_t s, e;
    char t[49], l[41];
    bool a;
    int c;
} ev_t;

static struct {
    esp_gsp_handle_t ui;
    calendar_face_ids_t id;
    uint16_t current;
    uint16_t *img;
    ev_t ev[MAX_EV];
    int n;
    bool have, fetch_failed, not_set;
    int64_t got_us, last_req_us;
    portMUX_TYPE lock;
    char *json;
    bool json_new;
    calendar_next_cb_t next_cb;
} k = { .lock = portMUX_INITIALIZER_UNLOCKED };

/* ---- pixels ---------------------------------------------------------------- */

static inline uint16_t rgb565(int r, int g, int b)
{
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline uint16_t mix(uint16_t d, int r, int g, int b, float a)
{
    if (a <= 0) {
        return d;
    }
    if (a > 1) {
        a = 1;
    }
    const int dr = (d >> 11) << 3, dg = ((d >> 5) & 0x3F) << 2, db = (d & 0x1F) << 3;
    return rgb565(dr + (int)((r - dr) * a), dg + (int)((g - dg) * a), db + (int)((b - db) * a));
}

/* Anti-aliased horizontal capsule from x0 to x1 (centres), radius r, at row cy. */
static void capsule(float x0, float x1, float cy, float r, int cr, int cg, int cb, float alpha)
{
    const int ya = (int)(cy - r - 1), yb = (int)(cy + r + 2);
    const int xa = (int)(x0 - r - 1), xb = (int)(x1 + r + 2);
    for (int y = ya < 0 ? 0 : ya; y < yb && y < FW; ++y) {
        for (int x = xa < 0 ? 0 : xa; x < xb && x < FW; ++x) {
            const float px = (float)x + 0.5f, py = (float)y + 0.5f;
            const float qx = px < x0 ? x0 : (px > x1 ? x1 : px);
            const float dx = px - qx, dy = py - cy;
            float d = __builtin_sqrtf(dx * dx + dy * dy) - r;
            const float a = 0.5f - d;
            if (a > 0) {
                k.img[y * FW + x] = mix(k.img[y * FW + x], cr, cg, cb, (a > 1 ? 1 : a) * alpha);
            }
        }
    }
}

static const int CAL_RGB[][3] = { { 92, 156, 255 }, { 88, 204, 140 }, { 232, 160, 58 }, { 200, 120, 230 } };

/* ---- time helpers ---------------------------------------------------------- */

static bool time_ok(time_t now)
{
    return now > 1700000000;   /* SNTP has set the clock */
}

static int day_index(time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_year * 400 + tm.tm_yday;
}

static void fmt_clock(time_t t, char *out, size_t len, bool short_ampm)
{
    struct tm tm;
    localtime_r(&t, &tm);
    const int h = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
    const char *ap = tm.tm_hour < 12 ? (short_ampm ? "a" : " AM") : (short_ampm ? "p" : " PM");
    if (tm.tm_min) {
        snprintf(out, len, "%d:%02d%s", h, tm.tm_min, ap);
    } else {
        snprintf(out, len, "%d%s", h, ap);
    }
}

/* Index of the current or next timed event, or -1. */
static int hero_index(time_t now)
{
    for (int i = 0; i < k.n; ++i) {
        if (!k.ev[i].a && k.ev[i].e > now) {
            return i;
        }
    }
    return -1;
}

/* ---- canvas ---------------------------------------------------------------- */

static void render(void)
{
    if (!k.img) {
        return;
    }
    for (int y = 0; y < FW; ++y) {
        const float t = (float)y / FW;
        const uint16_t c = rgb565((int)(16 * (1 - t)), (int)(22 * (1 - t)), (int)(44 * (1 - t)) + 4);
        uint16_t *row = k.img + y * FW;
        for (int x = 0; x < FW; ++x) {
            row[x] = c;
        }
    }
    const time_t now = time(NULL);
    const int h = time_ok(now) ? hero_index(now) : -1;
    if (h >= 0) {
        const ev_t *e = &k.ev[h];
        capsule(BAR_X0, BAR_X1, BAR_Y, 4, 255, 255, 255, 0.12f);
        float frac = -1;
        int r = 92, g = 156, b = 255;
        if (e->s <= now && e->e > e->s) {
            frac = (float)(now - e->s) / (float)(e->e - e->s);   /* elapsed */
        } else if (e->s - now <= 3600) {
            frac = 1.0f - (float)(e->s - now) / 3600.0f;          /* the last hour before it starts */
            r = 232, g = 160, b = 58;
        }
        if (frac > 0) {
            capsule(BAR_X0, BAR_X0 + (BAR_X1 - BAR_X0) * (frac > 1 ? 1 : frac), BAR_Y, 4, r, g, b, 1.0f);
        }
    }
    capsule(60, 420, 244, 0.6f, 255, 255, 255, 0.10f);   /* divider */
    /* A coloured dot per row for its calendar. */
    int row = 0;
    for (int i = h + 1; h >= 0 && i < k.n && row < ROWS; ++i) {
        if (k.ev[i].a) {
            continue;
        }
        const int *c = CAL_RGB[k.ev[i].c & 3];
        capsule(181, 181, ROW_Y0 + row * ROW_DY + 14, 4.5f, c[0], c[1], c[2], 1.0f);
        ++row;
    }
}

static void draw_cb(const esp_gsp_canvas_surface_t *s, void *ctx)
{
    (void)ctx;
    if (s->pixel_format != ESP_GSP_CANVAS_PIXEL_RGB565 || !k.img) {
        return;
    }
    uint16_t *px = s->pixels;
    const int stride = (int)(s->stride_bytes / 2);
    for (int y = 0; y < s->height; ++y) {
        memcpy(px + y * stride, k.img + (s->y + y) * FW + s->x, (size_t)s->width * 2);
    }
}

/* ---- data ------------------------------------------------------------------ */

static bool parse(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    bool ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"));
    k.not_set = !ok;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "ev");
    if (ok && cJSON_IsArray(arr)) {
        int n = 0;
        const cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            if (n >= MAX_EV) {
                break;
            }
            ev_t *e = &k.ev[n];
            const cJSON *v;
            v = cJSON_GetObjectItemCaseSensitive(it, "s");
            e->s = cJSON_IsNumber(v) ? (time_t)v->valuedouble : 0;
            v = cJSON_GetObjectItemCaseSensitive(it, "e");
            e->e = cJSON_IsNumber(v) ? (time_t)v->valuedouble : e->s;
            v = cJSON_GetObjectItemCaseSensitive(it, "t");
            snprintf(e->t, sizeof(e->t), "%s", cJSON_IsString(v) ? v->valuestring : "");
            v = cJSON_GetObjectItemCaseSensitive(it, "l");
            snprintf(e->l, sizeof(e->l), "%s", cJSON_IsString(v) ? v->valuestring : "");
            e->a = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "a"));
            v = cJSON_GetObjectItemCaseSensitive(it, "c");
            e->c = cJSON_IsNumber(v) ? v->valueint : 0;
            if (e->s) {
                ++n;
            }
        }
        k.n = n;
        k.have = true;
    }
    cJSON_Delete(root);
    return ok;
}

static void set(uint16_t bind, const char *text)
{
    esp_gsp_set_text(k.ui, bind, text);
}

static void update_labels(void)
{
    char buf[96], a[24], b[24];
    const time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (time_ok(now)) {
        strftime(a, sizeof(a), "%A", &tm);
        strftime(b, sizeof(b), "%b", &tm);
        snprintf(buf, sizeof(buf), "%s · %s %d", a, b, tm.tm_mday);
        set(k.id.date, buf);
    } else {
        set(k.id.date, "");
    }
    for (int i = 0; i < ROWS; ++i) {
        set(k.id.rt[i], "");
        set(k.id.rn[i], "");
    }
    if (!k.have) {
        set(k.id.allday, "");
        set(k.id.when, k.not_set ? "No calendar" : (k.fetch_failed ? "Offline" : "loading"));
        set(k.id.title, "");
        set(k.id.sub, k.not_set ? "add a calendar link on the relay" : "");
        set(k.id.upd, "");
        return;
    }
    /* All-day events covering today. */
    int n_all = 0;
    const char *first_all = NULL;
    for (int i = 0; i < k.n; ++i) {
        if (k.ev[i].a && k.ev[i].s <= now + 60 && k.ev[i].e > now) {
            if (!first_all) {
                first_all = k.ev[i].t;
            }
            ++n_all;
        }
    }
    if (first_all && n_all > 1) {
        snprintf(buf, sizeof(buf), "%s +%d", first_all, n_all - 1);
    } else {
        snprintf(buf, sizeof(buf), "%s", first_all ? first_all : "");
    }
    set(k.id.allday, buf);

    const int h = time_ok(now) ? hero_index(now) : -1;
    if (h < 0) {
        set(k.id.when, "All clear");
        set(k.id.title, "");
        set(k.id.sub, "nothing in the next week");
    } else {
        const ev_t *e = &k.ev[h];
        if (e->s <= now) {
            const int left = (int)((e->e - now + 59) / 60);
            if (left >= 90) {
                snprintf(buf, sizeof(buf), "Now · %d h %d min left", left / 60, left % 60);
            } else {
                snprintf(buf, sizeof(buf), "Now · %d min left", left);
            }
        } else {
            const int mins = (int)((e->s - now + 59) / 60);
            const int dd = day_index(e->s) - day_index(now);
            fmt_clock(e->s, a, sizeof(a), false);
            if (mins < 60) {
                snprintf(buf, sizeof(buf), "in %d min", mins);
            } else if (dd == 0 && mins < 6 * 60) {
                snprintf(buf, sizeof(buf), "in %d h %d min", mins / 60, mins % 60);
            } else if (dd == 0) {
                snprintf(buf, sizeof(buf), "at %s", a);
            } else if (dd == 1) {
                snprintf(buf, sizeof(buf), "Tomorrow %s", a);
            } else {
                struct tm st;
                localtime_r(&e->s, &st);
                strftime(b, sizeof(b), "%a", &st);
                snprintf(buf, sizeof(buf), "%s %s", b, a);
            }
        }
        set(k.id.when, buf);
        set(k.id.title, e->t[0] ? e->t : "Busy");
        fmt_clock(e->s, a, sizeof(a), false);
        fmt_clock(e->e, b, sizeof(b), false);
        /* "2:30-3:30 PM" when both share AM/PM. */
        const size_t la = strlen(a), lb = strlen(b);
        if (la > 3 && lb > 3 && strcmp(a + la - 3, b + lb - 3) == 0) {
            a[la - 3] = '\0';
        }
        if (e->l[0]) {
            snprintf(buf, sizeof(buf), "%s-%s · %s", a, b, e->l);
        } else {
            snprintf(buf, sizeof(buf), "%s-%s", a, b);
        }
        set(k.id.sub, buf);
        int row = 0;
        for (int i = h + 1; i < k.n && row < ROWS; ++i) {
            const ev_t *r = &k.ev[i];
            if (r->a) {
                continue;
            }
            const int dd = day_index(r->s) - day_index(now);
            fmt_clock(r->s, a, sizeof(a), dd != 0);
            if (dd == 0) {
                snprintf(buf, sizeof(buf), "%s", a);
            } else {
                struct tm st;
                localtime_r(&r->s, &st);
                strftime(b, sizeof(b), "%a", &st);
                snprintf(buf, sizeof(buf), "%s %s", b, a);
            }
            set(k.id.rt[row], buf);
            set(k.id.rn[row], r->t[0] ? r->t : "Busy");
            ++row;
        }
    }
    const int age = (int)((esp_timer_get_time() - k.got_us) / 60000000LL);
    if (k.fetch_failed && age >= 15) {
        snprintf(buf, sizeof(buf), "%d min old · relay unreachable", age);
    } else if (age >= 1) {
        snprintf(buf, sizeof(buf), "updated %d min ago", age);
    } else {
        snprintf(buf, sizeof(buf), "just updated");
    }
    set(k.id.upd, buf);
}

/* The eyes-screen line: "Now: Standup" / "Next 2:30 PM: Standup" / "". */
static void update_note(void)
{
    if (!k.next_cb) {
        return;
    }
    static char last[64] = "\x01";
    char buf[64] = "", a[16];
    const time_t now = time(NULL);
    const int h = (k.have && time_ok(now)) ? hero_index(now) : -1;
    if (h >= 0) {
        const ev_t *e = &k.ev[h];
        if (e->s <= now) {
            snprintf(buf, sizeof(buf), "Now: %s", e->t);
        } else if (e->s - now <= NOTE_AHEAD_S) {
            const int mins = (int)((e->s - now + 59) / 60);
            if (mins <= 60) {
                snprintf(buf, sizeof(buf), "In %d min: %s", mins, e->t);
            } else {
                fmt_clock(e->s, a, sizeof(a), false);
                snprintf(buf, sizeof(buf), "Next %s: %s", a, e->t);
            }
        }
    }
    if (strcmp(buf, last) != 0) {
        snprintf(last, sizeof(last), "%s", buf);
        k.next_cb(buf);
    }
}

static void on_calendar(const char *json, void *ctx)
{
    (void)ctx;
    portENTER_CRITICAL(&k.lock);
    if (json) {
        strlcpy(k.json, json, JSON_MAX);
    } else {
        k.json[0] = '\0';
    }
    k.json_new = true;
    portEXIT_CRITICAL(&k.lock);
}

static void request(int64_t now)
{
    k.last_req_us = now;
    app_voice_request_calendar();
}

static void tick(esp_gsp_handle_t ui, void *ctx)
{
    (void)ctx;
    const int64_t now = esp_timer_get_time();
    const bool visible = k.current == k.id.scene;
    if (!k.last_req_us && now > BOOT_FETCH_US) {
        request(now);
    } else if (k.last_req_us && now - k.last_req_us > (k.have && !k.fetch_failed ? REFRESH_US : RETRY_US)) {
        request(now);
    }
    static char local[JSON_MAX];
    bool fresh = false;
    portENTER_CRITICAL(&k.lock);
    if (k.json_new) {
        k.json_new = false;
        fresh = true;
        strlcpy(local, k.json, sizeof(local));
    }
    portEXIT_CRITICAL(&k.lock);
    if (fresh) {
        if (local[0] && parse(local)) {
            k.fetch_failed = false;
            k.got_us = now;
        } else if (!local[0]) {
            k.fetch_failed = true;
            ESP_LOGW(TAG, "calendar fetch failed");
        }
    }
    static int64_t last_lbl;
    if (fresh || now - last_lbl > 30LL * 1000 * 1000) {
        last_lbl = now;
        if (visible) {
            render();
            esp_gsp_canvas_invalidate(ui, k.id.canvas);
            update_labels();
        }
        update_note();
    }
}

void calendar_face_scene_changed(uint16_t scene_id)
{
    k.current = scene_id;
    if (scene_id != k.id.scene || !k.img) {
        return;
    }
    esp_gsp_canvas_set_draw_cb(k.ui, k.id.canvas, draw_cb, NULL);
    render();
    update_labels();
    const int64_t now = esp_timer_get_time();
    if (!k.have || now - k.got_us > 60LL * 1000 * 1000) {
        request(now);   /* a minute-old copy is fine; older, ask again */
    }
}

void calendar_face_set_next_handler(calendar_next_cb_t cb)
{
    k.next_cb = cb;
}

esp_err_t calendar_face_start(esp_gsp_handle_t ui, const calendar_face_ids_t *ids)
{
    k.ui = ui;
    k.id = *ids;
    k.img = heap_caps_malloc(FW * FW * 2, MALLOC_CAP_SPIRAM);
    k.json = heap_caps_calloc(1, JSON_MAX, MALLOC_CAP_SPIRAM);
    if (!k.img || !k.json) {
        return ESP_ERR_NO_MEM;
    }
    render();
    app_voice_set_calendar_handler(on_calendar, NULL);
    esp_gsp_timer_create(ui, 1000, tick, NULL);
    ESP_LOGI(TAG, "calendar screen ready");
    return ESP_OK;
}
