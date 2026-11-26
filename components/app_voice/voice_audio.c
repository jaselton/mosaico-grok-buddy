// SPDX-License-Identifier: Apache-2.0
/*
 * ES8311 audio for the voice service. The codec runs at the BSP default
 * (48 kHz, 16-bit stereo, shared by mic and speaker). Capture is mixed to mono
 * and decimated 3:1 to 16 kHz for the uplink. 24 kHz downlink audio waits in
 * a small jitter buffer, is upsampled 2x with a windowed-sinc FIR (keeps the
 * treble that linear interpolation loses), passes a small-speaker voicing
 * filter (high-pass + presence shelf + soft limiter) and is duplicated to
 * both channels. Buffers live in PSRAM.
 */
#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "bsp/esp_mosaico.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "app_wake.h"
#include "voice_internal.h"

static const char *TAG = "voice_audio";

#define CODEC_RATE      BSP_AUDIO_DEFAULT_SAMPLE_RATE   /* 48000 */
#define CODEC_CHANNELS  BSP_AUDIO_DEFAULT_CHANNELS      /* 2 */
#define FRAME_MS        20
#define CODEC_FRAMES    (CODEC_RATE * FRAME_MS / 1000)              /* 960 */
#define UP_SAMPLES      (VOICE_UPLINK_RATE * FRAME_MS / 1000)       /* 320 */
#define DOWN_SAMPLES    (VOICE_DOWNLINK_RATE * FRAME_MS / 1000)     /* 480 */
#define CAPTURE_SECONDS 6
#define PLAYBACK_SECONDS 40
#define MIC_GAIN_DB     30.0f
#define SPEAKER_VOLUME  82

_Static_assert(CODEC_RATE == 3 * VOICE_UPLINK_RATE, "capture decimation assumes 48k -> 16k");
_Static_assert(CODEC_RATE == 2 * VOICE_DOWNLINK_RATE, "playback upsamples 24k -> 48k");

/* Jitter buffer: start (or resume after running dry) only once this much audio
 * is queued, or once the oldest queued audio has waited JITTER_MAX_WAIT_MS
 * (so a short tail still plays). Prevents choppy speech on network jitter. */
#define JITTER_START_MS    500    /* initial fill before speaking */
#define JITTER_RESUME_MS   400    /* refill after running dry mid-reply */
#define JITTER_MAX_WAIT_MS 700    /* a short tail still plays after this long */
#define JITTER_START_BYTES (VOICE_DOWNLINK_RATE * 2 * JITTER_START_MS / 1000)
#define JITTER_RESUME_BYTES (VOICE_DOWNLINK_RATE * 2 * JITTER_RESUME_MS / 1000)

/* Playback diagnostics for the current reply (reset when playback goes idle). */
static atomic_int s_underruns;

/* 2x interpolation FIR: Blackman-windowed sinc, cutoff 11 kHz at 48 kHz. */
#define UPS_TAPS   48                 /* even: 24 per polyphase branch */
#define UPS_PHASE  (UPS_TAPS / 2)
#define UPS_CUTOFF 11000.0f
static float s_ups_h[UPS_TAPS];
static float s_ups_hist[UPS_PHASE];   /* newest input sample at index 0 */

static void ups_init(void)
{
    const float fc = UPS_CUTOFF / (float)CODEC_RATE;
    const float mid = (UPS_TAPS - 1) / 2.0f;
    for (int i = 0; i < UPS_TAPS; ++i) {
        const float t = i - mid;
        const float sinc = 2.0f * fc * (t == 0 ? 1.0f : sinf(2.0f * (float)M_PI * fc * t) / (2.0f * (float)M_PI * fc * t));
        const float w = 0.42f - 0.5f * cosf(2.0f * (float)M_PI * i / (UPS_TAPS - 1)) +
                        0.08f * cosf(4.0f * (float)M_PI * i / (UPS_TAPS - 1));
        s_ups_h[i] = 2.0f * sinc * w;   /* x2 restores the level lost to zero-stuffing */
    }
    memset(s_ups_hist, 0, sizeof(s_ups_hist));
}

/* Push one 24 kHz sample; produce two 48 kHz samples. */
static inline void ups_run(float x, float *y0, float *y1)
{
    memmove(&s_ups_hist[1], &s_ups_hist[0], sizeof(float) * (UPS_PHASE - 1));
    s_ups_hist[0] = x;
    float a = 0, b = 0;
    for (int k = 0; k < UPS_PHASE; ++k) {
        a += s_ups_h[2 * k] * s_ups_hist[k];
        b += s_ups_h[2 * k + 1] * s_ups_hist[k];
    }
    *y0 = a;
    *y1 = b;
}

/* Small-speaker voicing. The speaker can't reproduce low bass (it only muddies
 * speech), and its treble rolls off, so trim lows and lift the presence band. */
#define EQ_HPF_HZ      150.0f
#define EQ_SHELF_HZ    2800.0f
#define EQ_SHELF_DB    6.0f
#define EQ_PREGAIN     0.6f     /* headroom for the shelf boost */

typedef struct {
    float b0, b1, b2, a1, a2;   /* normalized by a0 */
    float z1, z2;
} biquad_t;

static biquad_t s_hpf;
static biquad_t s_shelf;

static void biquad_set(biquad_t *q, float b0, float b1, float b2, float a0, float a1, float a2)
{
    q->b0 = b0 / a0;
    q->b1 = b1 / a0;
    q->b2 = b2 / a0;
    q->a1 = a1 / a0;
    q->a2 = a2 / a0;
    q->z1 = q->z2 = 0;
}

static void eq_init(void)
{
    const float fs = (float)CODEC_RATE;   /* EQ runs after upsampling */
    /* RBJ cookbook high-pass, Q = 0.707. */
    float w0 = 2.0f * (float)M_PI * EQ_HPF_HZ / fs;
    float cw = cosf(w0);
    float alpha = sinf(w0) / (2.0f * 0.7071f);
    biquad_set(&s_hpf, (1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
    /* RBJ cookbook high shelf, slope 1. */
    const float A = powf(10.0f, EQ_SHELF_DB / 40.0f);
    w0 = 2.0f * (float)M_PI * EQ_SHELF_HZ / fs;
    cw = cosf(w0);
    alpha = sinf(w0) / 2.0f * sqrtf(2.0f);
    const float sa = 2.0f * sqrtf(A) * alpha;
    biquad_set(&s_shelf,
               A * ((A + 1) + (A - 1) * cw + sa), -2 * A * ((A - 1) + (A + 1) * cw),
               A * ((A + 1) + (A - 1) * cw - sa),
               (A + 1) - (A - 1) * cw + sa, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - sa);
}

static inline float biquad_run(biquad_t *q, float x)
{
    /* Transposed direct form II. */
    const float y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

static inline int16_t voice_sample(float in)
{
    float x = in * EQ_PREGAIN;
    x = biquad_run(&s_shelf, biquad_run(&s_hpf, x));
    /* Soft limiter above ~75% full scale instead of hard clipping. */
    const float knee = 24000.0f;
    const float ax = fabsf(x);
    if (ax > knee) {
        const float over = (ax - knee) / (32767.0f - knee);
        x = copysignf(knee + (32767.0f - knee) * tanhf(over), x);
    }
    return (int16_t)x;
}

static esp_codec_dev_handle_t s_mic;
static esp_codec_dev_handle_t s_spk;
static StreamBufferHandle_t s_capture;
static StreamBufferHandle_t s_playback;
static atomic_bool s_capturing;
static atomic_bool s_flush;
static atomic_bool s_playing;
static voice_level_cb_t s_level_cb;
static void *s_level_ctx;

static uint8_t level_from_pcm(const int16_t *pcm, size_t n)
{
    if (!n) {
        return 0;
    }
    double acc = 0;
    for (size_t i = 0; i < n; ++i) {
        acc += (double)pcm[i] * pcm[i];
    }
    const double rms = sqrt(acc / (double)n);
    if (rms < 1.0) {
        return 0;
    }
    /* Map -50..0 dBFS onto 0..255. */
    double db = 20.0 * log10(rms / 32768.0);
    double v = (db + 50.0) / 50.0;
    v = v < 0 ? 0 : (v > 1 ? 1 : v);
    return (uint8_t)(v * 255.0);
}

static StreamBufferHandle_t psram_stream(size_t bytes)
{
    uint8_t *storage = heap_caps_malloc(bytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StaticStreamBuffer_t *meta = heap_caps_malloc(sizeof(*meta), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!storage || !meta) {
        heap_caps_free(storage);
        heap_caps_free(meta);
        return NULL;
    }
    return xStreamBufferCreateStatic(bytes + 1, 1, storage, meta);
}

static void capture_task(void *arg)
{
    (void)arg;
    static int16_t in[CODEC_FRAMES * CODEC_CHANNELS];
    static int16_t out[UP_SAMPLES];
    for (;;) {
        if (esp_codec_dev_read(s_mic, in, sizeof(in)) != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
            continue;
        }
        for (int i = 0; i < UP_SAMPLES; ++i) {
            int32_t acc = 0;
            for (int k = 0; k < 3; ++k) {
                const int16_t *f = &in[(i * 3 + k) * CODEC_CHANNELS];
                acc += (f[0] + f[CODEC_CHANNELS - 1]) / 2;
            }
            out[i] = (int16_t)(acc / 3);
        }
        app_wake_feed(out, UP_SAMPLES);   /* no-op without a model or while paused */
        if (atomic_load(&s_capturing)) {
            xStreamBufferSend(s_capture, out, sizeof(out), 0);
            if (s_level_cb && !atomic_load(&s_playing)) {
                s_level_cb(level_from_pcm(out, UP_SAMPLES), s_level_ctx);
            }
        }
    }
}

static void playback_task(void *arg)
{
    (void)arg;
    static int16_t in[DOWN_SAMPLES];
    static int16_t out[CODEC_FRAMES * CODEC_CHANNELS];
    bool streaming = false;       /* false while (re)filling the jitter buffer */
    bool mid_reply = false;       /* ran dry during a reply: use the resume threshold */
    int64_t last_audio_us = 0;
    int64_t waiting_since_us = 0; /* when queued audio was first seen while not streaming */
    for (;;) {
        if (atomic_exchange(&s_flush, false)) {
            if (xStreamBufferReset(s_playback) != pdPASS) {
                uint8_t sink[256];
                while (xStreamBufferReceive(s_playback, sink, sizeof(sink), 0) > 0) {
                }
            }
            s_hpf.z1 = s_hpf.z2 = s_shelf.z1 = s_shelf.z2 = 0;
            memset(s_ups_hist, 0, sizeof(s_ups_hist));
            streaming = false;
            waiting_since_us = 0;
        }
        if (!streaming) {
            const size_t avail = xStreamBufferBytesAvailable(s_playback);
            if (!avail) {
                /* Quiet for over a second: the reply is over. The final run-dry is
                 * the normal end, so only earlier ones were audible gaps. */
                if (mid_reply && esp_timer_get_time() - last_audio_us > 1000LL * 1000) {
                    const int gaps = atomic_exchange(&s_underruns, 0) - 1;
                    if (gaps > 0) {
                        ESP_LOGW(TAG, "reply had %d playback gap(s): audio arrived slower than real time", gaps);
                    } else {
                        ESP_LOGI(TAG, "reply played without gaps");
                    }
                    mid_reply = false;
                }
                waiting_since_us = 0;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            const int64_t now = esp_timer_get_time();
            if (!waiting_since_us) {
                waiting_since_us = now;
            }
            const size_t need = mid_reply ? JITTER_RESUME_BYTES : JITTER_START_BYTES;
            if (avail < need && now - waiting_since_us < (int64_t)JITTER_MAX_WAIT_MS * 1000) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            streaming = true;
            waiting_since_us = 0;
        }
        const size_t got = xStreamBufferReceive(s_playback, in, sizeof(in), pdMS_TO_TICKS(40));
        if (got < sizeof(int16_t)) {
            /* Ran dry: refill the jitter buffer before resuming. If audio stops for
             * over a second the reply is over; report and reset the diagnostics. */
            streaming = false;
            if (last_audio_us) {
                atomic_fetch_add(&s_underruns, 1);
                mid_reply = true;
            }
            if (atomic_exchange(&s_playing, false) && s_level_cb) {
                s_level_cb(0, s_level_ctx);
            }
            continue;
        }
        atomic_store(&s_playing, true);
        last_audio_us = esp_timer_get_time();
        const size_t n = got / sizeof(int16_t);
        if (s_level_cb) {
            s_level_cb(level_from_pcm(in, n), s_level_ctx);
        }
        for (size_t i = 0; i < n; ++i) {
            float y0, y1;
            ups_run((float)in[i], &y0, &y1);
            const int16_t v0 = voice_sample(y0);
            const int16_t v1 = voice_sample(y1);
            int16_t *f = &out[i * 2 * CODEC_CHANNELS];
            for (int c = 0; c < CODEC_CHANNELS; ++c) {
                f[c] = v0;
                f[CODEC_CHANNELS + c] = v1;
            }
        }
        esp_codec_dev_write(s_spk, out, (int)(n * 2 * CODEC_CHANNELS * sizeof(int16_t)));
    }
}

esp_err_t voice_audio_start(voice_level_cb_t level_cb, void *ctx)
{
    s_level_cb = level_cb;
    s_level_ctx = ctx;
    eq_init();
    ups_init();
    s_capture = psram_stream(VOICE_UPLINK_RATE * 2 * CAPTURE_SECONDS);
    s_playback = psram_stream(VOICE_DOWNLINK_RATE * 2 * PLAYBACK_SECONDS);
    if (!s_capture || !s_playback) {
        return ESP_ERR_NO_MEM;
    }
    s_mic = bsp_audio_codec_microphone_init();
    s_spk = bsp_audio_codec_speaker_init();
    if (!s_mic || !s_spk) {
        ESP_LOGE(TAG, "codec init failed");
        return ESP_FAIL;
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = CODEC_RATE,
        .bits_per_sample = 16,
        .channel = CODEC_CHANNELS,
        .channel_mask = 0,
    };
    if (esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK ||
            esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open failed");
        return ESP_FAIL;
    }
    esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);
    esp_codec_dev_set_out_vol(s_spk, SPEAKER_VOLUME);
    if (xTaskCreate(capture_task, "voice_mic", 4096, NULL, 6, NULL) != pdPASS ||
            xTaskCreate(playback_task, "voice_spk", 4096, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "audio ready: codec %d Hz x%d, uplink %d Hz, downlink %d Hz", CODEC_RATE,
             CODEC_CHANNELS, VOICE_UPLINK_RATE, VOICE_DOWNLINK_RATE);
    return ESP_OK;
}

void voice_audio_capture_begin(void)
{
    atomic_store(&s_capturing, false);
    uint8_t sink[256];
    while (xStreamBufferReceive(s_capture, sink, sizeof(sink), 0) > 0) {
    }
    atomic_store(&s_capturing, true);
}

void voice_audio_capture_end(void)
{
    atomic_store(&s_capturing, false);
}

size_t voice_audio_capture_read(uint8_t *out, size_t max_bytes, uint32_t wait_ms)
{
    return xStreamBufferReceive(s_capture, out, max_bytes, pdMS_TO_TICKS(wait_ms));
}

void voice_audio_play(const uint8_t *pcm, size_t bytes)
{
    const size_t sent = xStreamBufferSend(s_playback, pcm, bytes & ~(size_t)1, pdMS_TO_TICKS(50));
    if (sent < bytes) {
        ESP_LOGW(TAG, "playback buffer full; dropped %u bytes", (unsigned)(bytes - sent));
    }
    if (sent) {
        atomic_store(&s_playing, true);
    }
}

void voice_audio_flush_playback(void)
{
    atomic_store(&s_flush, true);
}

bool voice_audio_playing(void)
{
    return atomic_load(&s_playing) || xStreamBufferBytesAvailable(s_playback) > 0;
}

void voice_audio_chime(void)
{
    /* Two soft rising tones (E6 then B6), 120 ms each with a short fade. */
    static const float freqs[] = { 1318.5f, 1975.5f };
    const int n = VOICE_DOWNLINK_RATE * 120 / 1000;
    const int fade = VOICE_DOWNLINK_RATE * 15 / 1000;
    int16_t *buf = heap_caps_malloc((size_t)n * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        return;
    }
    for (size_t t = 0; t < sizeof(freqs) / sizeof(freqs[0]); ++t) {
        for (int i = 0; i < n; ++i) {
            float env = 1.0f;
            if (i < fade) {
                env = (float)i / fade;
            } else if (i > n - fade) {
                env = (float)(n - i) / fade;
            }
            buf[i] = (int16_t)(6000.0f * env * sinf(2.0f * 3.14159265f * freqs[t] * i / VOICE_DOWNLINK_RATE));
        }
        voice_audio_play((const uint8_t *)buf, (size_t)n * sizeof(int16_t));
    }
    heap_caps_free(buf);
}
