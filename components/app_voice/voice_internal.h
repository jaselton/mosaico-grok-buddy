// SPDX-License-Identifier: Apache-2.0
/* Private interfaces between the voice service's Wi-Fi, audio and session parts. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Wire formats agreed with the xAI realtime session (PCM16 LE mono). */
#define VOICE_UPLINK_RATE   16000
#define VOICE_DOWNLINK_RATE 24000   /* half the bandwidth; upsampled 2x with a FIR */

/* ---- Wi-Fi station (voice_wifi.c) ---- */

/** Start the station with the given credentials and keep it reconnecting. */
esp_err_t voice_wifi_start(const char *ssid, const char *password, const char *backup_ssid,
                           const char *backup_password);
/** Wait up to timeout_ms for an IP address; true when connected. */
bool voice_wifi_wait_connected(uint32_t timeout_ms);

/* ---- Audio (voice_audio.c) ---- */

typedef void (*voice_level_cb_t)(uint8_t level, void *ctx);

/** Open the codec and start the capture and playback tasks. */
esp_err_t voice_audio_start(voice_level_cb_t level_cb, void *ctx);
/** Room level (0..255) every 20 ms while not capturing or playing; NULL to stop. */
void voice_audio_set_ambient_cb(voice_level_cb_t cb, void *ctx);
/** Begin buffering 16 kHz mic audio for the uplink (clears stale audio). */
void voice_audio_capture_begin(void);
/** Stop buffering mic audio. */
void voice_audio_capture_end(void);
/** Read up to max_bytes of captured 16 kHz PCM; waits up to wait_ms. */
size_t voice_audio_capture_read(uint8_t *out, size_t max_bytes, uint32_t wait_ms);
/** Queue 24 kHz PCM for playback; drops what does not fit. */
void voice_audio_play(const uint8_t *pcm, size_t bytes);
/** Discard queued playback (barge-in). */
void voice_audio_flush_playback(void);
/** True while queued playback remains. */
bool voice_audio_playing(void);
/** Queue a short two-tone chime ahead of an announcement. */
void voice_audio_chime(void);

/* ---- Grok Bot relay (voice_bot.c) ---- */

#define VOICE_CALL_ID_MAX 64

typedef enum {
    VOICE_BOT_SENT,          /* call_id: task accepted by the relay */
    VOICE_BOT_SEND_FAILED,   /* call_id: relay or Wi-Fi unavailable */
    VOICE_BOT_RESULT,        /* text: the bot's spoken answer */
    VOICE_BOT_ATTENTION,     /* text: the bot is waiting on the owner (card, choice) */
} voice_bot_event_t;

/** Called on the relay task; must copy text and return quickly. */
typedef void (*voice_bot_cb_t)(voice_bot_event_t ev, const char *call_id, const char *text, void *ctx);

/** Start the relay client (base URL without trailing path, device token). */
esp_err_t voice_bot_start(const char *base_url, const char *token, voice_bot_cb_t cb, void *ctx);
/** Queue a task for the bot; false if the queue is full or out of memory. */
bool voice_bot_send(const char *call_id, const char *task);

/* Usage summary (relay GET /summary), fetched on the bot task. json is NULL on failure. */
typedef void (*voice_summary_cb_t)(const char *json, void *ctx);
void voice_bot_set_summary_cb(voice_summary_cb_t cb, void *ctx);
void voice_bot_request_summary(void);

