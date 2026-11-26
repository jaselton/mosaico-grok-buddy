// SPDX-License-Identifier: Apache-2.0
/*
 * Grok Bot desk buddy eyes: idle (blink + glances), listening, thinking, speaking.
 * Portable between the ESP-Mosaico device and the GSP PC simulator.
 * Eye shapes are pre-rendered Grok Bot-style frames (fan-made)
 * (see tools/render_eyes.py); this controller only moves and swaps them.
 */
#include "app_buddy.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "bundle_gsp.h"
#ifdef ESP_PLATFORM
#include "esp_log.h"
static const char *TAG = "app_buddy";
#define BUDDY_LOG(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#include <stdio.h>
#define BUDDY_LOG(...) (printf(__VA_ARGS__), printf("\n"))
#endif

#define TICK_MS 33
#define FRAME_COUNT 5            /* open, think, b1, b2, b3 */
#define BAR_COUNT 9
#define DOT_COUNT 3
#define WAVE_H 140
#define BAR_MIN_H 18
#define BAR_MAX_H 132

/* Authored defaults in ui/main.json (container top-left, 160x160 frames). */
#define EYE_L_X 139
#define EYE_L_Y 118
#define EYE_R_X 267
#define EYE_R_Y 58
#define EYE_RANGE 46             /* scene bounds are +-48 */

enum { F_OPEN = 0, F_THINK, F_B1, F_B2, F_B3 };

#define DIZZY_MS   2600
#define HAPPY_MS   2200
#define STARTLE_MS 900
/* Woozy sequence (ms from start): fast spin, winding down, hard blinks,
 * shaking it off, settling back. */
#define WOOZY_FAST_END   1500
#define WOOZY_SLOW_END   3500
#define WOOZY_BLINK_END  5000
#define WOOZY_SHAKE_END  6000
#define WOOZY_MS         6500

typedef esp_gsp_err_t (*vis_fn_t)(esp_gsp_handle_t, bool);

static const vis_fn_t s_left[FRAME_COUNT] = {
    gsp_buddy_eye_l_open_set_visible, gsp_buddy_eye_l_think_set_visible,
    gsp_buddy_eye_l_b1_set_visible, gsp_buddy_eye_l_b2_set_visible,
    gsp_buddy_eye_l_b3_set_visible,
};
static const vis_fn_t s_right[FRAME_COUNT] = {
    gsp_buddy_eye_r_open_set_visible, gsp_buddy_eye_r_think_set_visible,
    gsp_buddy_eye_r_b1_set_visible, gsp_buddy_eye_r_b2_set_visible,
    gsp_buddy_eye_r_b3_set_visible,
};
typedef esp_gsp_err_t (*int_fn_t)(esp_gsp_handle_t, int32_t);

/* Generated typed setters (runtime timer callbacks run on the render task). */
static const int_fn_t s_bar_h[BAR_COUNT] = {
    gsp_buddy_bar_0_set_h, gsp_buddy_bar_1_set_h, gsp_buddy_bar_2_set_h,
    gsp_buddy_bar_3_set_h, gsp_buddy_bar_4_set_h, gsp_buddy_bar_5_set_h,
    gsp_buddy_bar_6_set_h, gsp_buddy_bar_7_set_h, gsp_buddy_bar_8_set_h,
};
static const int_fn_t s_bar_y[BAR_COUNT] = {
    gsp_buddy_bar_0_set_y, gsp_buddy_bar_1_set_y, gsp_buddy_bar_2_set_y,
    gsp_buddy_bar_3_set_y, gsp_buddy_bar_4_set_y, gsp_buddy_bar_5_set_y,
    gsp_buddy_bar_6_set_y, gsp_buddy_bar_7_set_y, gsp_buddy_bar_8_set_y,
};
static const int_fn_t s_dot_alpha[DOT_COUNT] = {
    gsp_buddy_dot_0_set_opacity, gsp_buddy_dot_1_set_opacity, gsp_buddy_dot_2_set_opacity,
};

/* Blink: close fast, open a little slower (frames per 33 ms tick). */
static const uint8_t s_blink_seq[] = { F_B1, F_B2, F_B3, F_B3, F_B2, F_B1 };
#define BLINK_LEN (sizeof(s_blink_seq) / sizeof(s_blink_seq[0]))

struct app_buddy {
    esp_gsp_handle_t ui;
    void *timer;
    app_buddy_state_t state;
    uint32_t t_ms;
    uint32_t frames, blinks, taps;
    uint32_t rng;
    /* eyes */
    int shown_frame;
    int blink_pos;               /* -1 = not blinking */
    uint32_t next_blink_ms;
    bool double_blink;
    float gx, gy;                /* current glance offset */
    float tx, ty;                /* glance target */
    uint32_t next_glance_ms;
    int last_dx, last_dy;
    /* speaking */
    float bar_h[BAR_COUNT];
    float bar_target[BAR_COUNT];
    int bar_shown[BAR_COUNT];
    /* thinking */
    int dot_shown[DOT_COUNT];
    esp_gsp_err_t last_error;
    /* Cross-task inputs, consumed on the render task. -1 = no pending state. */
    atomic_int pending_state;
    atomic_int level;            /* 0..255 */
    atomic_bool has_level;
    atomic_bool busy;            /* a background task is running: show slow dots when idle */
    bool dots_on;
    float level_smooth;
    app_buddy_tap_cb_t tap_cb;
    void *tap_ctx;
    app_buddy_tap_cb_t pet_cb;
    void *pet_ctx;
    uint32_t ignore_tap_until_ms;    /* a long press is followed by its release click */
    /* Reactions, sleep and tilt (cross-task inputs, consumed on the render task). */
    atomic_int pending_react;        /* -1 none */
    atomic_bool asleep;
    atomic_bool alarm;
    atomic_int tilt_x, tilt_y;       /* -100..100 */
    int react;                       /* active reaction or -1 */
    uint32_t react_start_ms, react_until_ms;
    float react_phase;               /* woozy spin angle, radians */
    bool was_asleep;
};

static void note(struct app_buddy *s, esp_gsp_err_t err)
{
    if (err != ESP_GSP_OK) {
        s->last_error = err;
    }
}

static uint32_t rnd(struct app_buddy *s)
{
    s->rng = s->rng * 1664525u + 1013904223u;
    return s->rng >> 8;
}

static int rnd_range(struct app_buddy *s, int lo, int hi)
{
    return lo + (int)(rnd(s) % (uint32_t)(hi - lo + 1));
}

static void show_frame(struct app_buddy *s, int frame)
{
    if (frame == s->shown_frame) {
        return;
    }
    /* Show the new frame first so the eye never disappears for a render. */
    note(s, s_left[frame](s->ui, true));
    note(s, s_right[frame](s->ui, true));
    if (s->shown_frame >= 0) {
        note(s, s_left[s->shown_frame](s->ui, false));
        note(s, s_right[s->shown_frame](s->ui, false));
    }
    s->shown_frame = frame;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void move_eyes(struct app_buddy *s, int dx, int dy)
{
    dx = clampi(dx, -EYE_RANGE, EYE_RANGE);
    dy = clampi(dy, -EYE_RANGE, EYE_RANGE);
    if (dx == s->last_dx && dy == s->last_dy) {
        return;
    }
    note(s, esp_gsp_component_set_position(s->ui, GSP_BUDDY_OBJ_KEY_EYE_L, EYE_L_X + dx, EYE_L_Y + dy));
    note(s, esp_gsp_component_set_position(s->ui, GSP_BUDDY_OBJ_KEY_EYE_R, EYE_R_X + dx, EYE_R_Y + dy));
    s->last_dx = dx;
    s->last_dy = dy;
}

static void schedule_blink(struct app_buddy *s)
{
    s->next_blink_ms = s->t_ms + (uint32_t)rnd_range(s, 2200, 5600);
}

static void schedule_glance(struct app_buddy *s)
{
    s->next_glance_ms = s->t_ms + (uint32_t)rnd_range(s, 1400, 4200);
}

static void pick_glance(struct app_buddy *s)
{
    if (rnd(s) % 100 < 45) {
        s->tx = 0;           /* back to centre */
        s->ty = 0;
    } else {
        s->tx = (float)rnd_range(s, -30, 30);
        s->ty = (float)rnd_range(s, -16, 12);
    }
}

static void set_bar(struct app_buddy *s, int i, int h)
{
    h = clampi(h, BAR_MIN_H, BAR_MAX_H);
    if (h == s->bar_shown[i]) {
        return;
    }
    note(s, s_bar_h[i](s->ui, h));
    note(s, s_bar_y[i](s->ui, (WAVE_H - h) / 2));
    s->bar_shown[i] = h;
}

static void set_dot(struct app_buddy *s, int i, int alpha)
{
    alpha = clampi(alpha, 0, 255);
    if (alpha == s->dot_shown[i]) {
        return;
    }
    note(s, s_dot_alpha[i](s->ui, alpha));
    s->dot_shown[i] = alpha;
}

static void enter_state(struct app_buddy *s, app_buddy_state_t st)
{
    s->state = st;
    note(s, gsp_buddy_wave_set_visible(s->ui, st == APP_BUDDY_SPEAKING || st == APP_BUDDY_LISTENING));
    s->dots_on = st == APP_BUDDY_THINKING;
    note(s, gsp_buddy_dots_set_visible(s->ui, s->dots_on));
    if (st == APP_BUDDY_IDLE) {
        s->tx = 0;
        s->ty = 0;
        schedule_glance(s);
    }
    for (int i = 0; i < BAR_COUNT; ++i) {
        s->bar_h[i] = BAR_MIN_H;
        s->bar_target[i] = BAR_MIN_H;
    }
}

static void tick_bars(struct app_buddy *s)
{
    const float t = (float)s->t_ms;
    /* Syllable-like envelope: two slow sines plus jitter, centre bars taller. */
    float env = 0.55f + 0.30f * sinf(t / 260.0f) + 0.15f * sinf(t / 97.0f);
    if (env < 0.08f) {
        env = 0.08f;
    }
    for (int i = 0; i < BAR_COUNT; ++i) {
        if (s->frames % 3 == (uint32_t)(i % 3)) {
            const float c = (float)(i - (BAR_COUNT - 1) / 2) / ((BAR_COUNT - 1) / 2.0f);
            const float w = 1.0f - 0.55f * c * c;
            const float j = (float)(rnd(s) % 100) / 100.0f;
            s->bar_target[i] = BAR_MIN_H + (BAR_MAX_H - BAR_MIN_H) * env * w * (0.45f + 0.55f * j);
        }
        s->bar_h[i] += (s->bar_target[i] - s->bar_h[i]) * 0.45f;
        set_bar(s, i, (int)lroundf(s->bar_h[i]));
    }
}

/* Bars from a live level: a centre-weighted shape with per-bar jitter. */
static void tick_bars_level(struct app_buddy *s)
{
    const float target = (float)atomic_load(&s->level) / 255.0f;
    /* Fast attack, slower release so speech reads as syllables. */
    const float k = target > s->level_smooth ? 0.6f : 0.18f;
    s->level_smooth += (target - s->level_smooth) * k;
    for (int i = 0; i < BAR_COUNT; ++i) {
        if (s->frames % 2 == (uint32_t)(i % 2)) {
            const float c = (float)(i - (BAR_COUNT - 1) / 2) / ((BAR_COUNT - 1) / 2.0f);
            const float w = 1.0f - 0.55f * c * c;
            const float j = (float)(rnd(s) % 100) / 100.0f;
            s->bar_target[i] = BAR_MIN_H + (BAR_MAX_H - BAR_MIN_H) * s->level_smooth * w * (0.6f + 0.4f * j);
        }
        s->bar_h[i] += (s->bar_target[i] - s->bar_h[i]) * 0.5f;
        set_bar(s, i, (int)lroundf(s->bar_h[i]));
    }
}

static void tick_dots(struct app_buddy *s)
{
    const float phase = (float)(s->t_ms % 1200) / 1200.0f * 6.2831853f;
    for (int i = 0; i < DOT_COUNT; ++i) {
        float v = sinf(phase - (float)i * 0.9f);
        if (v < 0) {
            v = 0;
        }
        set_dot(s, i, 70 + (int)(185.0f * v));
    }
}

static void tick(esp_gsp_handle_t ui, void *ctx)
{
    (void)ui;
    struct app_buddy *s = ctx;
    s->t_ms += TICK_MS;
    s->frames++;
    const float t = (float)s->t_ms;

    const int pending = atomic_exchange(&s->pending_state, -1);
    if (pending >= 0 && pending != (int)s->state) {
        enter_state(s, (app_buddy_state_t)pending);
    }

    const int react_req = atomic_exchange(&s->pending_react, -1);
    if (react_req >= 0) {
        s->react = react_req;
        s->react_start_ms = s->t_ms;
        s->react_phase = 0;
        s->react_until_ms = s->t_ms + (react_req == APP_BUDDY_REACT_DIZZY  ? DIZZY_MS
                                       : react_req == APP_BUDDY_REACT_HAPPY ? HAPPY_MS
                                       : react_req == APP_BUDDY_REACT_WOOZY ? WOOZY_MS
                                                                            : STARTLE_MS);
    }
    if (s->react >= 0 && s->t_ms >= s->react_until_ms) {
        s->react = -1;
    }
    const bool asleep = atomic_load(&s->asleep);
    if (asleep != s->was_asleep) {
        s->was_asleep = asleep;
        if (asleep) {
            note(s, gsp_buddy_wave_set_visible(s->ui, false));
            note(s, gsp_buddy_dots_set_visible(s->ui, false));
            s->dots_on = false;
        } else {
            enter_state(s, s->state);   /* restore bars/dots for the current state */
            s->react = APP_BUDDY_REACT_STARTLE;
            s->react_start_ms = s->t_ms;
            s->react_until_ms = s->t_ms + STARTLE_MS;
        }
    }
    if (atomic_load(&s->alarm)) {
        /* Alarm: wide eyes darting with quick worried blinks (overrides sleep). */
        const uint32_t ph = s->t_ms % 700;
        show_frame(s, ph < 60 ? F_B2 : (ph < 110 ? F_B1 : F_OPEN));
        move_eyes(s, (int)lroundf(7.0f * sinf(t / 45.0f)), -12 + (int)lroundf(3.0f * sinf(t / 70.0f)));
        return;
    }
    if (asleep) {
        /* Eyes closed, drooped, slow breathing; nothing else runs. */
        show_frame(s, F_B3);
        move_eyes(s, 0, 16 + (int)lroundf(3.0f * sinf(t / 900.0f)));
        return;
    }
    if (s->react >= 0 && s->state == APP_BUDDY_IDLE) {
        const float k = (float)(s->t_ms - s->react_start_ms);
        float rdx = 0, rdy = 0;
        int rframe = F_OPEN;
        switch (s->react) {
        case APP_BUDDY_REACT_DIZZY: {
            const float r = 26.0f * (1.0f - k / DIZZY_MS);
            rdx = r * cosf(k / 90.0f);
            rdy = r * sinf(k / 90.0f);
            rframe = ((int)(k / 400.0f) % 3 == 1) ? F_B1 : F_OPEN;
            break;
        }
        case APP_BUDDY_REACT_HAPPY:
            rdy = -10.0f + 4.0f * sinf(k / 140.0f);
            rframe = F_B1;   /* happy squint */
            break;
        case APP_BUDDY_REACT_WOOZY:
            if (k < WOOZY_SLOW_END) {
                /* Spin: ~4 turns/s, then wind down to ~0.4 turns/s while the circle tightens. */
                float omega = 0.025f;   /* rad per ms */
                float r = 28.0f;
                if (k >= WOOZY_FAST_END) {
                    const float u = (k - WOOZY_FAST_END) / (float)(WOOZY_SLOW_END - WOOZY_FAST_END);
                    omega = 0.025f + (0.0025f - 0.025f) * u;
                    r = 28.0f - 14.0f * u;
                }
                s->react_phase += omega * TICK_MS;
                rdx = r * cosf(s->react_phase);
                rdy = r * sinf(s->react_phase);
                rframe = F_OPEN;
            } else if (k < WOOZY_BLINK_END) {
                /* Three hard blinks: squeezed shut 300 ms, open 200 ms, eyes drooping. */
                const uint32_t b = (uint32_t)(k - WOOZY_SLOW_END) % 500;
                rframe = b < 40 ? F_B2 : (b < 300 ? F_B3 : (b < 340 ? F_B2 : F_OPEN));
                rdy = 8.0f;
            } else if (k < WOOZY_SHAKE_END) {
                /* Shake it off: fast side-to-side that dies away. */
                const float u = (k - WOOZY_BLINK_END) / (float)(WOOZY_SHAKE_END - WOOZY_BLINK_END);
                rdx = 20.0f * (1.0f - u) * sinf((k - WOOZY_BLINK_END) / 22.0f);
                rdy = 8.0f * (1.0f - u);
                rframe = F_OPEN;
            } else {
                /* Settle: one normal blink back to the idle face. */
                rframe = (k - WOOZY_SHAKE_END) < 120 ? F_B1 : F_OPEN;
            }
            break;
        default:   /* STARTLE */
            rdy = -22.0f * expf(-k / 250.0f);
            rframe = k < 120 ? F_B2 : F_OPEN;
            break;
        }
        show_frame(s, rframe);
        s->gx = rdx;
        s->gy = rdy;
        move_eyes(s, (int)lroundf(rdx), (int)lroundf(rdy));
        return;
    }

    /* Blinks run in every state. */
    int base = (s->state == APP_BUDDY_THINKING) ? F_THINK : F_OPEN;
    int frame = base;
    if (s->blink_pos < 0 && s->t_ms >= s->next_blink_ms) {
        s->blink_pos = 0;
        s->blinks++;
        s->double_blink = (rnd(s) % 100) < 18;
    }
    if (s->blink_pos >= 0) {
        frame = s_blink_seq[s->blink_pos++];
        if (s->blink_pos >= (int)BLINK_LEN) {
            if (s->double_blink) {
                s->double_blink = false;
                s->blink_pos = 1;
            } else {
                s->blink_pos = -1;
                schedule_blink(s);
            }
        }
    }
    show_frame(s, frame);

    float dx = 0, dy = 0;
    switch (s->state) {
    case APP_BUDDY_IDLE: {
        /* Busy: slow, dim dots under idle eyes until the background task finishes. */
        const bool want = atomic_load(&s->busy);
        if (want != s->dots_on) {
            s->dots_on = want;
            note(s, gsp_buddy_dots_set_visible(s->ui, want));
        }
        if (want) {
            const float ph = (float)(s->t_ms % 2400) / 2400.0f * 6.2831853f;
            for (int i = 0; i < DOT_COUNT; ++i) {
                float v = sinf(ph - (float)i * 0.9f);
                set_dot(s, i, 40 + (int)(110.0f * (v < 0 ? 0 : v)));
            }
        }
        if (s->t_ms >= s->next_glance_ms) {
            pick_glance(s);
            schedule_glance(s);
        }
        {
            /* A tilted head pulls the gaze downhill, overriding idle glances. */
            const int tx = atomic_load(&s->tilt_x);
            const int ty = atomic_load(&s->tilt_y);
            if (tx * tx + ty * ty > 15 * 15) {
                s->tx = (float)tx * 0.42f;
                s->ty = (float)ty * 0.42f;
                s->next_glance_ms = s->t_ms + 1500;
            }
        }
        /* Quick saccade toward the target, then hold. */
        s->gx += (s->tx - s->gx) * 0.28f;
        s->gy += (s->ty - s->gy) * 0.28f;
        dx = s->gx;
        dy = s->gy;
        break;
    }
    case APP_BUDDY_LISTENING:
        /* Look straight at the user, slightly raised, with a tiny attentive drift. */
        s->gx += (0.0f - s->gx) * 0.25f;
        s->gy += (-6.0f - s->gy) * 0.25f;
        dx = s->gx + 2.0f * sinf(t / 900.0f);
        dy = s->gy;
        tick_bars_level(s);
        break;
    case APP_BUDDY_THINKING:
        s->tx = 24.0f + 10.0f * sinf(t / 650.0f);
        s->ty = -26.0f + 5.0f * sinf(t / 1050.0f);
        s->gx += (s->tx - s->gx) * 0.12f;
        s->gy += (s->ty - s->gy) * 0.12f;
        dx = s->gx;
        dy = s->gy;
        tick_dots(s);
        break;
    case APP_BUDDY_SPEAKING:
    default:
        s->gx += (0.0f - s->gx) * 0.2f;
        s->gy += (-10.0f - s->gy) * 0.2f;
        dx = s->gx;
        dy = s->gy + 4.0f * sinf(t / 170.0f);
        if (atomic_load(&s->has_level)) {
            tick_bars_level(s);
        } else {
            tick_bars(s);
        }
        break;
    }
    move_eyes(s, (int)lroundf(dx), (int)lroundf(dy));
}

static void on_event(esp_gsp_handle_t ui, const esp_gsp_event_t *event, void *ctx)
{
    (void)ui;
    struct app_buddy *s = ctx;
    if (!gsp_buddy_event_is_tap(event)) {
        return;
    }
    BUDDY_LOG("touch: %s", event->arg == 1 ? "long press (pet)" : "tap");
    if (event->arg == 1) {   /* authored long-press call: petting */
        s->ignore_tap_until_ms = s->t_ms + 800;
        atomic_store(&s->pending_react, APP_BUDDY_REACT_HAPPY);
        if (s->pet_cb) {
            s->pet_cb(s->pet_ctx);
        }
        return;
    }
    if (s->t_ms < s->ignore_tap_until_ms) {
        return;
    }
    s->taps++;
    if (s->tap_cb) {
        s->tap_cb(s->tap_ctx);
        return;
    }
    enter_state(s, (app_buddy_state_t)((s->state + 1) % APP_BUDDY_STATE_COUNT));
}

esp_gsp_err_t app_buddy_request_state(app_buddy_handle_t handle, app_buddy_state_t state)
{
    if (!handle || state >= APP_BUDDY_STATE_COUNT) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    atomic_store(&handle->pending_state, (int)state);
    return ESP_GSP_OK;
}

void app_buddy_set_level(app_buddy_handle_t handle, uint8_t level)
{
    if (!handle) {
        return;
    }
    atomic_store(&handle->level, (int)level);
    atomic_store(&handle->has_level, true);
}

void app_buddy_react(app_buddy_handle_t handle, app_buddy_reaction_t reaction)
{
    if (handle) {
        atomic_store(&handle->pending_react, (int)reaction);
    }
}

void app_buddy_set_alarm(app_buddy_handle_t handle, bool ringing)
{
    if (handle) {
        atomic_store(&handle->alarm, ringing);
    }
}

void app_buddy_set_sleep(app_buddy_handle_t handle, bool asleep)
{
    if (handle) {
        atomic_store(&handle->asleep, asleep);
    }
}

static int clamp100(int v)
{
    return v < -100 ? -100 : (v > 100 ? 100 : v);
}

void app_buddy_set_tilt(app_buddy_handle_t handle, int x, int y)
{
    if (handle) {
        atomic_store(&handle->tilt_x, clamp100(x));
        atomic_store(&handle->tilt_y, clamp100(y));
    }
}

void app_buddy_set_pet_handler(app_buddy_handle_t handle, app_buddy_tap_cb_t cb, void *ctx)
{
    if (handle) {
        handle->pet_ctx = ctx;
        handle->pet_cb = cb;
    }
}

void app_buddy_set_busy(app_buddy_handle_t handle, bool busy)
{
    if (handle) {
        atomic_store(&handle->busy, busy);
    }
}

void app_buddy_set_tap_handler(app_buddy_handle_t handle, app_buddy_tap_cb_t cb, void *ctx)
{
    if (!handle) {
        return;
    }
    handle->tap_ctx = ctx;
    handle->tap_cb = cb;
}

esp_gsp_err_t app_buddy_set_state(app_buddy_handle_t handle, app_buddy_state_t state)
{
    if (!handle || state >= APP_BUDDY_STATE_COUNT) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    enter_state(handle, state);
    return ESP_GSP_OK;
}

esp_gsp_err_t app_buddy_create(esp_gsp_handle_t ui, app_buddy_handle_t *out_handle)
{
    if (!out_handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    if (!ui) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    struct app_buddy *s = calloc(1, sizeof(*s));
    if (!s) {
        return ESP_GSP_ERR_NO_MEM;
    }
    s->ui = ui;
    s->rng = 0x6b6f7275u;
    s->shown_frame = F_OPEN;     /* scene starts with the open frames visible */
    s->blink_pos = -1;
    atomic_init(&s->pending_state, -1);
    atomic_init(&s->level, 0);
    atomic_init(&s->has_level, false);
    atomic_init(&s->busy, false);
    atomic_init(&s->pending_react, -1);
    atomic_init(&s->asleep, false);
    atomic_init(&s->alarm, false);
    atomic_init(&s->tilt_x, 0);
    atomic_init(&s->tilt_y, 0);
    s->react = -1;
    s->last_dx = 0;
    s->last_dy = 0;
    for (int i = 0; i < BAR_COUNT; ++i) {
        s->bar_shown[i] = -1;
    }
    for (int i = 0; i < DOT_COUNT; ++i) {
        s->dot_shown[i] = -1;
    }
    note(s, esp_gsp_set_press_feedback_enabled(ui, false));
    enter_state(s, APP_BUDDY_IDLE);
    s->next_blink_ms = 1200;
    esp_gsp_err_t err = s->last_error;
    if (err == ESP_GSP_OK) {
        err = esp_gsp_on_event(ui, on_event, s);
    }
    if (err == ESP_GSP_OK) {
        s->timer = esp_gsp_timer_create(ui, TICK_MS, tick, s);
        if (!s->timer) {
            (void)esp_gsp_on_event(ui, NULL, NULL);
            err = ESP_GSP_ERR_NO_MEM;
        }
    }
    if (err != ESP_GSP_OK) {
        free(s);
        return err;
    }
    *out_handle = s;
    return ESP_GSP_OK;
}

esp_gsp_err_t app_buddy_read(app_buddy_handle_t handle, app_buddy_snapshot_t *out)
{
    if (!handle || !out) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out = (app_buddy_snapshot_t) {
        .state = handle->state, .frames = handle->frames, .blinks = handle->blinks,
        .taps = handle->taps, .last_error = handle->last_error,
    };
    return ESP_GSP_OK;
}

esp_gsp_err_t app_buddy_destroy(app_buddy_handle_t *handle)
{
    if (!handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    struct app_buddy *s = *handle;
    if (!s) {
        return ESP_GSP_OK;
    }
    esp_gsp_err_t err = esp_gsp_on_event(s->ui, NULL, NULL);
    if (err != ESP_GSP_OK) {
        return err;
    }
    if (s->timer) {
        err = esp_gsp_timer_delete(s->ui, s->timer);
        if (err != ESP_GSP_OK) {
            return err;
        }
        s->timer = NULL;
    }
    free(s);
    *handle = NULL;
    return ESP_GSP_OK;
}
