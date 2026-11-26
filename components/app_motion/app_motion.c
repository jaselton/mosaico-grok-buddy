// SPDX-License-Identifier: Apache-2.0
/*
 * Motion and haptics for the desk buddy (device only).
 * One task samples the BMI270 at 25 Hz. "Upright" is whatever gravity
 * points at during the first second, so the buddy can stand or lie flat.
 *   shake     |accel| jumps >= 1.5 g          -> dizzy + buzz      (3 s cooldown)
 *   bump      |accel| deviates 0.35..1.5 g     -> startle           (4 s cooldown)
 *   tilt      gravity moves off "upright"      -> eyes look downhill
 *   face-down screen normal points at the desk -> sleep (1.2 s), wake on pick-up
 * Motor patterns run on the same task so callers never block.
 */
#include "app_motion.h"

#include <math.h>
#include <stdatomic.h>

#include "bsp/esp_mosaico.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_motion";

#define PERIOD_MS        40
#define CALIB_SAMPLES    25
#define SHAKE_G          1.5f
#define BUMP_G           0.35f
#define SHAKE_COOLDOWN_MS 3000
#define BUMP_COOLDOWN_MS  4000
#define FACE_DOWN_Z      -0.75f
#define FACE_UP_Z        -0.3f
#define SLEEP_HOLD_MS    1200
#define TILT_FULL_G      0.5f      /* this much gravity shift = full gaze offset */
#define AGITATE_G        0.5f      /* jolt that counts as "still shaking" */
#define AGITATE_GAP_MS   500       /* a calmer gap than this ends the shaking */
#define WOOZY_AFTER_MS   5000      /* sustained shaking that makes it woozy */
#define WOOZY_LOCK_MS    7000      /* no other reactions while woozy plays */
#define PURR_STRENGTH    55        /* percent */
#define BUZZ_STRENGTH    90

typedef enum { HAPTIC_NONE, HAPTIC_PURR, HAPTIC_BUZZ } haptic_t;

static struct {
    app_motion_config_t cfg;
    atomic_int haptic;
    atomic_bool alarm;
    bool motor_ok;
} m;

static void run_haptic(haptic_t h)
{
    if (!m.motor_ok || h == HAPTIC_NONE) {
        return;
    }
    /* Purr: three soft pulses. Buzz: one firm pulse. Note: in the BSP,
     * bsp_motor_set_strength() turns the motor ON at that level, so strength
     * is only ever set inside a pulse and the motor always ends at 0. */
    const int pulses = h == HAPTIC_PURR ? 3 : 1;
    const int on_ms = h == HAPTIC_PURR ? 45 : 120;
    const uint8_t strength = h == HAPTIC_PURR ? PURR_STRENGTH : BUZZ_STRENGTH;
    for (int i = 0; i < pulses; ++i) {
        bsp_motor_set_strength(strength);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        bsp_motor_set_strength(0);
        vTaskDelay(pdMS_TO_TICKS(70));
    }
}

static void motion_task(void *arg)
{
    (void)arg;
    float gx0 = 0, gy0 = 0, gz0 = 0;
    int calib = 0;
    float prev_mag = 1.0f;
    int64_t shake_ok_us = 0, bump_ok_us = 0, down_since_us = 0;
    int64_t agitated_since_us = 0, last_agitate_us = 0;
    bool woozy_fired = false;
    bool asleep = false;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        run_haptic((haptic_t)atomic_exchange(&m.haptic, HAPTIC_NONE));
        if (m.motor_ok) {
            /* Alarm: 300 ms on / 500 ms off, driven by the sample clock so motion
             * (shake to dismiss) keeps being read. */
            static bool buzzing;
            const bool want = atomic_load(&m.alarm) && (esp_timer_get_time() / 1000) % 800 < 300;
            if (want != buzzing) {
                buzzing = want;
                bsp_motor_set_strength(want ? BUZZ_STRENGTH : 0);
            }
        }
        float ax, ay, az;
        if (bsp_imu_get_accel(&ax, &ay, &az) != ESP_OK) {
            vTaskDelayUntil(&wake, pdMS_TO_TICKS(PERIOD_MS));
            continue;
        }
        const int64_t now = esp_timer_get_time();
        const float mag = sqrtf(ax * ax + ay * ay + az * az);
        if (calib < CALIB_SAMPLES) {
            gx0 += ax / CALIB_SAMPLES;
            gy0 += ay / CALIB_SAMPLES;
            gz0 += az / CALIB_SAMPLES;
            if (++calib == CALIB_SAMPLES) {
                ESP_LOGI(TAG, "upright = %+.2f %+.2f %+.2f g", gx0, gy0, gz0);
            }
            prev_mag = mag;
            vTaskDelayUntil(&wake, pdMS_TO_TICKS(PERIOD_MS));
            continue;
        }

        /* Face-down sleep, with hysteresis so it doesn't flicker. */
        if (!asleep && az <= FACE_DOWN_Z) {
            if (!down_since_us) {
                down_since_us = now;
            } else if ((now - down_since_us) / 1000 >= SLEEP_HOLD_MS) {
                asleep = true;
                ESP_LOGI(TAG, "face-down: sleeping");
                app_buddy_set_sleep(m.cfg.buddy, true);
                if (m.cfg.on_sleep) {
                    m.cfg.on_sleep(true, m.cfg.ctx);
                }
            }
        } else if (az > FACE_DOWN_Z) {
            down_since_us = 0;
        }
        if (asleep && az >= FACE_UP_Z) {
            asleep = false;
            ESP_LOGI(TAG, "picked up: awake");
            app_buddy_set_sleep(m.cfg.buddy, false);
            if (m.cfg.on_sleep) {
                m.cfg.on_sleep(false, m.cfg.ctx);
            }
            bump_ok_us = shake_ok_us = now + 1000LL * 1000;   /* waking motion isn't a bump */
        }

        if (!asleep) {
            const float jolt = fabsf(mag - prev_mag);
            /* Sustained shaking: jolts with no calm gap longer than AGITATE_GAP_MS. */
            if (jolt >= AGITATE_G || fabsf(mag - 1.0f) >= SHAKE_G * 0.6f) {
                if (!agitated_since_us || (now - last_agitate_us) / 1000 > AGITATE_GAP_MS) {
                    agitated_since_us = now;
                    woozy_fired = false;
                }
                last_agitate_us = now;
            } else if (agitated_since_us && (now - last_agitate_us) / 1000 > AGITATE_GAP_MS) {
                agitated_since_us = 0;
            }
            if (agitated_since_us && !woozy_fired && (now - agitated_since_us) / 1000 >= WOOZY_AFTER_MS) {
                woozy_fired = true;
                ESP_LOGI(TAG, "shaken for 5 s: woozy");
                app_buddy_react(m.cfg.buddy, APP_BUDDY_REACT_WOOZY);
                shake_ok_us = bump_ok_us = now + (int64_t)WOOZY_LOCK_MS * 1000;
            }
            if ((jolt >= SHAKE_G || mag >= 1.0f + SHAKE_G) && now >= shake_ok_us) {
                ESP_LOGI(TAG, "shake");
                if (m.cfg.on_shake) {
                    m.cfg.on_shake(m.cfg.ctx);
                }
                app_buddy_react(m.cfg.buddy, APP_BUDDY_REACT_DIZZY);
                atomic_store(&m.haptic, HAPTIC_BUZZ);
                shake_ok_us = now + (int64_t)SHAKE_COOLDOWN_MS * 1000;
                bump_ok_us = now + (int64_t)BUMP_COOLDOWN_MS * 1000;
            } else if (fabsf(mag - 1.0f) >= BUMP_G && jolt < SHAKE_G && now >= bump_ok_us) {
                ESP_LOGI(TAG, "bump");
                app_buddy_react(m.cfg.buddy, APP_BUDDY_REACT_STARTLE);
                bump_ok_us = now + (int64_t)BUMP_COOLDOWN_MS * 1000;
            }
            /* Tilt: gravity shift from upright in the screen plane (x right, y down). */
            const int tx = (int)lroundf((ax - gx0) / TILT_FULL_G * 100.0f);
            const int ty = (int)lroundf((ay - gy0) / TILT_FULL_G * 100.0f);
            app_buddy_set_tilt(m.cfg.buddy, tx, ty);
        }
        prev_mag = mag;
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(PERIOD_MS));
    }
}

void app_motion_set_alarm(bool ringing)
{
    atomic_store(&m.alarm, ringing);
}

void app_motion_purr(void)
{
    atomic_store(&m.haptic, HAPTIC_PURR);
}

esp_err_t app_motion_start(const app_motion_config_t *config)
{
    if (!config || !config->buddy) {
        return ESP_ERR_INVALID_ARG;
    }
    m.cfg = *config;
    esp_err_t err = bsp_imu_init();
    if (err == ESP_OK) {
        const bsp_imu_config_t imu = BSP_IMU_CONFIG_DEFAULT();
        err = bsp_imu_start(&imu);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IMU unavailable (%s); motion reactions off", esp_err_to_name(err));
        return err;
    }
    m.motor_ok = bsp_motor_init() == ESP_OK;
    if (m.motor_ok) {
        bsp_motor_set_strength(0);   /* make sure it starts off */
    }
    if (xTaskCreateWithCaps(motion_task, "motion", 4096, NULL, 3, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "motion reactions on (haptics %s)", m.motor_ok ? "on" : "off");
    return ESP_OK;
}
