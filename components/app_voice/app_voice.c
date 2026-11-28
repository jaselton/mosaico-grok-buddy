// SPDX-License-Identifier: Apache-2.0
/*
 * Tap-to-talk Grok voice session over xAI's realtime WebSocket API.
 *
 * Tap -> LISTENING: mic audio streams to Grok; server VAD ends the turn.
 * speech_stopped -> THINKING: mic stops (half duplex: the speaker would
 * otherwise be heard as the user). First audio delta -> SPEAKING.
 * Playback drained after response.done -> IDLE. Tapping while Grok talks
 * cancels the response and listens again. The socket opens on the first tap
 * and closes after IDLE_CLOSE_MS without activity.
 *
 * Threads: the control task owns all session state. WebSocket and tap
 * callbacks only post events to its queue; the uplink task only streams
 * captured audio while `s_uplink` is set.
 */
#include "app_voice.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "app_alarm.h"
#include "app_camera.h"
#include "app_workout.h"
#include "app_config.h"
#include "app_wake.h"
#include "sdkconfig.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"
#include "voice_internal.h"

static const char *TAG = "app_voice";

#define REALTIME_MODEL "grok-voice-3.0"   /* pinned: price and behaviour change only on purpose */
#define REALTIME_URI   "wss://api.x.ai/v1/realtime?model=" REALTIME_MODEL
#define DEFAULT_VOICE  "eve"
#define API_KEY_MAX    192
#define VOICE_ID_MAX   32
#define RX_MAX         (512 * 1024)      /* largest reassembled server message */
#define UPLINK_CHUNK   (VOICE_UPLINK_RATE * 2 / 10)   /* 100 ms of PCM16 */
#define LISTEN_NO_SPEECH_MS 5000   /* counted from when listening starts */
#define LISTEN_MAX_MS  30000
#define THINK_MAX_MS   20000
#define CONNECT_MAX_MS 10000
#define IDLE_CLOSE_MS  10000    /* voice is billed per minute; keep only a short follow-up window */

#define BOT_NAME       CONFIG_GROK_BUDDY_BOT_NAME     /* Kconfig string literals */
#define OWNER          CONFIG_GROK_BUDDY_OWNER_NAME
#define MAX_ANNOUNCE   4

#define INSTRUCTIONS                                                                \
    "You are Grok Bot, a witty, warm desk buddy with big expressive eyes, living "  \
    "on a small round screen on " OWNER "'s desk. You talk through a tiny speaker, " \
    "so keep answers short and conversational: one to three sentences unless asked " \
    "for more. Be playful, curious and direct. No lists or markdown; speak naturally."

#define BOT_INSTRUCTIONS                                                            \
    " " OWNER " also has a personal Grok Bot agent named " BOT_NAME " that has "    \
    "their accounts, files, email, calendar and memory and can take real actions. " \
    "When " OWNER " asks you to have " BOT_NAME " (or their bot) do something, or " \
    "asks for anything that needs their personal data or real-world actions, call " \
    "the ask_grok_bot tool with a complete, self-contained task. Then say in a few " \
    "words that " BOT_NAME " is on it; its answer will be read out when it "        \
    "arrives, which can take a few minutes. Don't use the tool for chit-chat or "  \
    "general knowledge you can answer yourself."

#define ATTENTION_PREFIX                                                            \
    "[" BOT_NAME ", " OWNER "'s Grok Bot, is waiting on " OWNER " for something in the " \
    "Grok Bot app. Tell " OWNER " now, in one or two short sentences, what is waiting " \
    "and that it needs them in the app. Don't add new information.]\n\n"

#define ALARM_INSTRUCTIONS                                                          \
    " You can set alarms and timers on this device with set_alarm: pass `time` "     \
    "(local 24-hour HH:MM) for a clock time, or `in_seconds` for anything relative " \
    "(\"in 10 minutes\" = 600; up to 99 minutes runs as the countdown timer on "  \
    "the timer screen, only one at a time). A clock-time alarm moves the blue alarm hand on "    \
    "the watch face; set `daily` true only if they ask for it every day. Add a "     \
    "short label if one was given. cancel_alarms clears them and switches the hand " \
    "off. Confirm briefly using the time the tool returns."

#define WORKOUT_INSTRUCTIONS                                                        \
    " There is a workout interval timer on its own screen. Use the workout tool "    \
    "for exercise timing: presets tabata (20 on/10 off x8), emom (every minute on "  \
    "the minute), interval (40/20), rounds (3 min on/1 off, boxing) and stopwatch "  \
    "(counts up). For anything custom pass work_seconds, rest_seconds and rounds "   \
    "(e.g. \"10 rounds of 45 on 15 off\"; an AMRAP of 12 minutes = work_seconds "   \
    "720, 1 round). It starts with a 10 second get-ready and beeps at every "       \
    "change, so keep your reply to a few words. Use timer_control to pause, "      \
    "resume, reset or check the plain countdown timer."

#define SCREEN_INSTRUCTIONS                                                         \
    " The device has swipeable screens, in order: eyes (your face, home), clock "   \
    "(world-time watch with the alarm hand and a chronograph), calendar (current or " \
    "next event with a countdown, then the ones after), weather (now, next "        \
    "five hours, pollen), forecast (five days), timer (countdown), workout "        \
    "(interval timer) and usage (Grok plan usage and voice minutes). When " OWNER    \
    " asks to see, open or go to one, or asks about their schedule, weather, the "   \
    "forecast or usage "                                                             \
    "while you answer, call show_screen; say just a few words since they can see "  \
    "it. Use get_weather for the numbers (it includes pollen and the coming days) "  \
    "and get_calendar for events."

#define LOOK_INSTRUCTIONS                                                           \
    " This device has a small camera. When " OWNER " asks what you see, to look at " \
    "something, read something or identify what they're holding, call look with "  \
    "their question; answer from what it returns, briefly. Only look when asked."

#define GREETING_PREFIX                                                             \
    "[" OWNER "'s alarm just rang and they switched it off. Greet them warmly; if it " \
    "is morning, give a quick briefing: today's date; after calling get_weather, "   \
    "today's weather in one sentence; and, if get_calendar is available, the first " \
    "thing on today's calendar. Mention the alarm label if there is one. Keep it "   \
    "to three short sentences.]\n\n"

#define BRIEFING_TASK                                                               \
    "Morning briefing for " OWNER ", who just woke up (their alarm rang). In two or "  \
    "three short spoken sentences: their first meetings or commitments today and "    \
    "anything urgent waiting on them. No lists, no URLs."

#define ANNOUNCE_PREFIX                                                             \
    "[Message from " BOT_NAME ", " OWNER "'s Grok Bot. Read it to " OWNER " now in " \
    "your own voice, briefly and naturally, starting with something like '"         \
    BOT_NAME " says'. Don't add new information.]\n\n"

typedef enum {
    EV_TAP,
    EV_STOP,
    EV_PREFS,
    EV_WS_CONNECTED,
    EV_WS_CLOSED,
    EV_SESSION_READY,
    EV_SPEECH_STARTED,
    EV_SPEECH_STOPPED,
    EV_RESPONSE_CREATED,
    EV_AUDIO,
    EV_RESPONSE_DONE,
    EV_ERROR,
    EV_BUDGET,          /* relay: daily voice budget used up */
    EV_SLEEP,           /* laid face-down */
    EV_WAKE_WORD,       /* "Hey Grok" heard */
    EV_LOCAL_TOOL,      /* text = "name\nargs-json", call_id set */
    EV_ALARM_RING,      /* text = label */
    EV_ALARM_STOP,      /* text = label */
    EV_WAKE,            /* picked up */
    EV_TOOL_CALL,       /* text = task, call_id set */
    EV_TOOL_SENT,       /* call_id set */
    EV_TOOL_FAILED,     /* call_id set */
    EV_BOT_RESULT,      /* text = bot answer */
    EV_BOT_ATTENTION,   /* text = what the bot is waiting on */
    EV_BEEP,            /* call_id[0] = '0' + app_voice_beep_t */
    EV_NIGHT,           /* call_id[0] = '1' night, '0' day */
    EV_LOOK,            /* text = question, call_id set */
    EV_LOOK_DONE,       /* text = tool output JSON, call_id set */
} event_t;

/* Queue item; text (if any) is heap-owned by the receiver. */
typedef struct {
    event_t ev;
    char *text;
    char call_id[VOICE_CALL_ID_MAX];
} msg_t;

typedef enum {
    V_IDLE,
    V_CONNECTING,   /* tap received; waiting for session.updated */
    V_LISTENING,
    V_THINKING,
    V_SPEAKING,
} vstate_t;

static const char *const STATE_NAMES[] = { "idle", "connecting", "listening", "thinking", "speaking" };

static struct {
    app_buddy_handle_t buddy;
    QueueHandle_t q;
    esp_websocket_client_handle_t ws;
    char uri[192];           /* relay /voice (preferred) or xAI realtime (direct) */
    char auth_header[API_KEY_MAX + 32];
    bool via_relay;
    int64_t budget_lock_until_us;   /* daily budget spent: no voice until then */
    bool asleep;                    /* face-down: mic off, taps ignored */
    bool alarm_ringing;
    int64_t next_chime_us;
    char voice[VOICE_ID_MAX];
    vstate_t state;
    bool ready;              /* session.updated received on the open socket */
    bool response_done;
    atomic_bool uplink;      /* uplink task may stream */
    atomic_bool discard;     /* drop audio from a cancelled response */
    int64_t state_since_us;
    int64_t last_activity_us;
    bool heard_speech;
    /* Receive reassembly, owned by the WebSocket task. */
    char *rx;
    size_t rx_len;
    bool rx_overflow;
    uint8_t *pcm;            /* decoded audio scratch, WebSocket task */
    size_t pcm_cap;
    /* Grok Bot hand-off (control task). */
    bool bot_enabled;
    bool tool_pending;       /* waiting for the relay to accept a tool call */
    char *announce[MAX_ANNOUNCE];   /* leading ATTN_MARK = attention notice */
    int announce_count;
    int pending_tasks;              /* tasks handed to the bot, not yet answered */
    bool night;                     /* night hours (from the app) */
    int64_t night_lift_until_us;    /* a tap or an alarm lifts night mode for a while */
    atomic_bool night_now;          /* night mode in effect (read from other tasks) */
    app_voice_screen_cb_t screen_cb;
    void *screen_ctx;
    int64_t pending_since_us;
} s;

#define ATTN_MARK '\x01'
#define GREET_MARK '\x02'
#define PENDING_MAX_MS (20 * 60 * 1000)

static char *dup_psram(const char *src)
{
    if (!src) {
        return NULL;
    }
    const size_t n = strlen(src) + 1;
    char *d = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (d) {
        memcpy(d, src, n);
    }
    return d;
}

static void post_msg(event_t ev, const char *text, const char *call_id)
{
    if (!s.q) {
        return;
    }
    msg_t m = { .ev = ev, .text = dup_psram(text) };
    if (call_id) {
        snprintf(m.call_id, sizeof(m.call_id), "%s", call_id);
    }
    if (xQueueSend(s.q, &m, 0) != pdTRUE) {
        heap_caps_free(m.text);
    }
}

static void post(event_t ev)
{
    post_msg(ev, NULL, NULL);
}

static void on_tap(void *ctx)
{
    (void)ctx;
    post(EV_TAP);
}

void app_voice_tap(void)
{
    post(EV_TAP);
}

void app_voice_stop(void)
{
    post(EV_STOP);
}

_Static_assert(sizeof(app_voice_wifi_ap_t) == sizeof(voice_wifi_ap_t), "wifi ap layout");
_Static_assert(APP_VOICE_WIFI_SCAN_MAX == VOICE_WIFI_SCAN_MAX, "wifi scan max");

esp_err_t app_voice_wifi_scan(app_voice_wifi_scan_cb_t cb, void *ctx)
{
    return voice_wifi_scan((voice_wifi_scan_cb_t)cb, ctx);
}

esp_err_t app_voice_wifi_join(const char *ssid, const char *password)
{
    return voice_wifi_join(ssid, password);
}

void app_voice_wifi_status(char *out, size_t len)
{
    voice_wifi_status(out, len);
}

void app_voice_set_volume(int percent)
{
    voice_audio_set_volume(percent);
}

static atomic_bool s_wake_off;

void app_voice_set_wake_word(bool on)
{
    atomic_store(&s_wake_off, !on);
    post(EV_PREFS);
}

static void on_ambient(uint8_t level, void *ctx)
{
    (void)ctx;
    app_buddy_set_ambient(s.buddy, level);
}

static void on_level(uint8_t level, void *ctx)
{
    (void)ctx;
    app_buddy_set_level(s.buddy, level);
}

static void face(app_buddy_state_t st)
{
    app_buddy_request_state(s.buddy, st);
}

static void update_wake_enable(void);

#define NIGHT_LIFT_TAP_US   (10LL * 60 * 1000 * 1000)
#define NIGHT_LIFT_ALARM_US (3LL * 3600 * 1000 * 1000)

static bool night_active(void)
{
    return s.night && !s.alarm_ringing && esp_timer_get_time() >= s.night_lift_until_us;
}

static void show_screen(app_voice_screen_t scr)
{
    if (s.screen_cb) {
        s.screen_cb(scr, s.screen_ctx);
    }
}

static void set_state(vstate_t st)
{
    if (st != s.state) {
        ESP_LOGI(TAG, "%s -> %s", STATE_NAMES[s.state], STATE_NAMES[st]);
    }
    s.state = st;
    s.state_since_us = esp_timer_get_time();
    s.last_activity_us = s.state_since_us;
    switch (st) {
    case V_IDLE:
        face(APP_BUDDY_IDLE);
        break;
    case V_CONNECTING:
    case V_LISTENING:
        face(APP_BUDDY_LISTENING);
        break;
    case V_THINKING:
        face(APP_BUDDY_THINKING);
        break;
    case V_SPEAKING:
        face(APP_BUDDY_SPEAKING);
        break;
    }
    update_wake_enable();
}

/* Listen for the wake word only when idle, awake and within budget (half duplex:
 * never while the speaker might be talking). */
static void update_wake_enable(void)
{
    app_wake_set_enabled(!atomic_load(&s_wake_off) && s.state == V_IDLE && !s.asleep && !s.alarm_ringing && !night_active() &&
                         esp_timer_get_time() >= s.budget_lock_until_us && !voice_audio_playing());
}

static void on_wake_word(void *ctx)
{
    (void)ctx;
    post(EV_WAKE_WORD);
}

static bool send_json(const char *json)
{
    if (!s.ws || !esp_websocket_client_is_connected(s.ws)) {
        return false;
    }
    const int n = esp_websocket_client_send_text(s.ws, json, (int)strlen(json), pdMS_TO_TICKS(2000));
    return n >= 0;
}

/* ---------- Server message handling (WebSocket task) ---------- */

static void handle_audio_delta(const char *b64)
{
    if (atomic_load(&s.discard) || !b64) {
        return;
    }
    const size_t in_len = strlen(b64);
    const size_t need = in_len / 4 * 3 + 3;
    if (need > s.pcm_cap) {
        uint8_t *p = heap_caps_realloc(s.pcm, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) {
            ESP_LOGE(TAG, "no memory for %u-byte audio delta", (unsigned)need);
            return;
        }
        s.pcm = p;
        s.pcm_cap = need;
    }
    size_t out_len = 0;
    if (mbedtls_base64_decode(s.pcm, s.pcm_cap, &out_len, (const unsigned char *)b64, in_len) != 0) {
        ESP_LOGW(TAG, "bad base64 audio delta");
        return;
    }
    voice_audio_play(s.pcm, out_len);
    post(EV_AUDIO);
}

/* Expressions from Grok's own words, as the transcript streams in: instant and
 * free (an emote tool would pause the audio for a round trip). First match wins. */
static const struct {
    const char *word;
    app_buddy_expr_t expr;
} EXPR_WORDS[] = {
    { "haha", APP_BUDDY_EXPR_LAUGH },     { "hah", APP_BUDDY_EXPR_LAUGH },
    { "lol", APP_BUDDY_EXPR_LAUGH },      { "laugh", APP_BUDDY_EXPR_LAUGH },
    { "funny", APP_BUDDY_EXPR_LAUGH },    { "hilarious", APP_BUDDY_EXPR_LAUGH },
    { "wow", APP_BUDDY_EXPR_SURPRISED },  { "whoa", APP_BUDDY_EXPR_SURPRISED },
    { "no way", APP_BUDDY_EXPR_SURPRISED }, { "seriously", APP_BUDDY_EXPR_SURPRISED },
    { "amazing", APP_BUDDY_EXPR_SURPRISED }, { "incredible", APP_BUDDY_EXPR_SURPRISED },
    { "love", APP_BUDDY_EXPR_LOVE },      { "adorable", APP_BUDDY_EXPR_LOVE },
    { "sorry", APP_BUDDY_EXPR_SAD },      { "unfortunately", APP_BUDDY_EXPR_SAD },
    { "oh no", APP_BUDDY_EXPR_SAD },      { "sad", APP_BUDDY_EXPR_SAD },
    { "hmm", APP_BUDDY_EXPR_CURIOUS },    { "interesting", APP_BUDDY_EXPR_CURIOUS },
    { "wonder", APP_BUDDY_EXPR_CURIOUS }, { "curious", APP_BUDDY_EXPR_CURIOUS },
    { "actually", APP_BUDDY_EXPR_SKEPTICAL }, { "doubt", APP_BUDDY_EXPR_SKEPTICAL },
    { "not so sure", APP_BUDDY_EXPR_SKEPTICAL }, { "suspicious", APP_BUDDY_EXPR_SKEPTICAL },
    { "great", APP_BUDDY_EXPR_HAPPY },    { "awesome", APP_BUDDY_EXPR_HAPPY },
    { "glad", APP_BUDDY_EXPR_HAPPY },     { "nice", APP_BUDDY_EXPR_HAPPY },
    { "congrat", APP_BUDDY_EXPR_HAPPY },  { "yay", APP_BUDDY_EXPR_HAPPY },
    { "happy", APP_BUDDY_EXPR_HAPPY },    { "fun", APP_BUDDY_EXPR_HAPPY },
};

static void express_from_transcript(const char *delta)
{
    /* Keep a short lowercase tail so words split across deltas still match. */
    static char tail[96];
    size_t tl = strlen(tail);
    for (const char *p = delta; *p; ++p) {
        if (tl + 1 >= sizeof(tail)) {
            memmove(tail, tail + 48, tl - 48 + 1);
            tl -= 48;
        }
        tail[tl++] = (char)tolower((unsigned char)*p);
        tail[tl] = '\0';
    }
    for (size_t i = 0; i < sizeof(EXPR_WORDS) / sizeof(EXPR_WORDS[0]); ++i) {
        char *hit = strstr(tail, EXPR_WORDS[i].word);
        if (hit) {
            app_buddy_express(s.buddy, EXPR_WORDS[i].expr);
            /* Consume through the match so it fires once. */
            memmove(tail, hit + strlen(EXPR_WORDS[i].word), strlen(hit + strlen(EXPR_WORDS[i].word)) + 1);
            return;
        }
    }
}

static void handle_message(const char *msg, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(msg, len);
    if (!root) {
        ESP_LOGW(TAG, "unparseable server message (%u bytes)", (unsigned)len);
        return;
    }
    const cJSON *type_j = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *type = cJSON_IsString(type_j) ? type_j->valuestring : "";

    if (!strcmp(type, "response.output_audio.delta") || !strcmp(type, "response.audio.delta")) {
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "delta");
        if (!cJSON_IsString(d)) {
            d = cJSON_GetObjectItemCaseSensitive(root, "audio");
        }
        handle_audio_delta(cJSON_IsString(d) ? d->valuestring : NULL);
    } else if (!strcmp(type, "response.function_call_arguments.done")) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
        const cJSON *call_id = cJSON_GetObjectItemCaseSensitive(root, "call_id");
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(root, "arguments");
        if (cJSON_IsString(name) && cJSON_IsString(args) &&
                (!strcmp(name->valuestring, "set_alarm") || !strcmp(name->valuestring, "cancel_alarms") ||
                 !strcmp(name->valuestring, "timer_control") || !strcmp(name->valuestring, "workout") ||
                 !strcmp(name->valuestring, "show_screen"))) {
            const size_t n = strlen(name->valuestring) + strlen(args->valuestring) + 2;
            char *t = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (t) {
                snprintf(t, n, "%s\n%s", name->valuestring, args->valuestring);
                post_msg(EV_LOCAL_TOOL, t, cJSON_IsString(call_id) ? call_id->valuestring : "");
                heap_caps_free(t);
            }
        } else if (cJSON_IsString(name) && !strcmp(name->valuestring, "look") && cJSON_IsString(args)) {
            cJSON *a = cJSON_Parse(args->valuestring);
            const cJSON *q = a ? cJSON_GetObjectItemCaseSensitive(a, "question") : NULL;
            post_msg(EV_LOOK, cJSON_IsString(q) ? q->valuestring : "What do you see?",
                     cJSON_IsString(call_id) ? call_id->valuestring : "");
            cJSON_Delete(a);
        } else if (cJSON_IsString(name) && !strcmp(name->valuestring, "ask_grok_bot") && cJSON_IsString(args)) {
            cJSON *a = cJSON_Parse(args->valuestring);
            const cJSON *task = a ? cJSON_GetObjectItemCaseSensitive(a, "task") : NULL;
            if (cJSON_IsString(task) && task->valuestring[0]) {
                ESP_LOGI(TAG, "handing to %s: %.160s", BOT_NAME, task->valuestring);
                post_msg(EV_TOOL_CALL, task->valuestring, cJSON_IsString(call_id) ? call_id->valuestring : "");
            }
            cJSON_Delete(a);
        }
    } else if (!strcmp(type, "session.created")) {
        app_buddy_set_status(s.buddy, "");   /* reached Grok: clear any old warning */
    } else if (!strcmp(type, "session.updated")) {
        post(EV_SESSION_READY);
    } else if (!strcmp(type, "input_audio_buffer.speech_started")) {
        post(EV_SPEECH_STARTED);
    } else if (!strcmp(type, "input_audio_buffer.speech_stopped")) {
        post(EV_SPEECH_STOPPED);
    } else if (!strcmp(type, "response.created")) {
        atomic_store(&s.discard, false);
        post(EV_RESPONSE_CREATED);
    } else if (!strcmp(type, "response.done")) {
        post(EV_RESPONSE_DONE);
    } else if (!strcmp(type, "response.output_audio_transcript.delta")) {
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "delta");
        if (cJSON_IsString(d)) {
            express_from_transcript(d->valuestring);
        }
    } else if (!strcmp(type, "response.output_audio_transcript.done")) {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "transcript");
        if (cJSON_IsString(t)) {
            ESP_LOGI(TAG, "grok said: %.200s", t->valuestring);
        }
    } else if (!strcmp(type, "conversation.item.input_audio_transcription.completed")) {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "transcript");
        if (cJSON_IsString(t)) {
            ESP_LOGI(TAG, "heard: %.200s", t->valuestring);
        }
    } else if (!strcmp(type, "error")) {
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(root, "error");
        const cJSON *code = e ? cJSON_GetObjectItemCaseSensitive(e, "code") : NULL;
        const cJSON *msg = e ? cJSON_GetObjectItemCaseSensitive(e, "message") : NULL;
        if (cJSON_IsString(code) && !strncmp(code->valuestring, "voice_", 6)) {
            /* Relay: xAI refused the session (billing, key, rate limit, outage). */
            ESP_LOGW(TAG, "voice unavailable: %s", code->valuestring);
            app_buddy_set_status(s.buddy, cJSON_IsString(msg) ? msg->valuestring : "Grok voice is down");
            app_buddy_express(s.buddy, APP_BUDDY_EXPR_SAD);
            post(EV_ERROR);
            cJSON_Delete(root);
            return;
        }
        if (cJSON_IsString(code) && !strcmp(code->valuestring, "daily_budget_exhausted")) {
            ESP_LOGW(TAG, "daily voice budget used up");
            app_buddy_set_status(s.buddy, "daily voice limit reached");
            post(EV_BUDGET);
            cJSON_Delete(root);
            return;
        }
        char *txt = cJSON_PrintUnformatted(root);
        ESP_LOGE(TAG, "server error: %.300s", txt ? txt : "?");
        cJSON_free(txt);
        post(EV_ERROR);
    } else {
        ESP_LOGD(TAG, "event %s", type);
    }
    cJSON_Delete(root);
}

static void ws_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *d = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "realtime socket connected");
        post(EV_WS_CONNECTED);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        post(EV_WS_CLOSED);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "socket error");
        post(EV_WS_CLOSED);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x2) {
            ESP_LOGD(TAG, "ignoring binary frame");
            break;
        }
        if (d->op_code != 0x1 && d->op_code != 0x0) {
            break;   /* ping/pong/close handled by the client */
        }
        if (d->op_code == 0x1 && d->payload_offset == 0) {
            s.rx_len = 0;
            s.rx_overflow = false;
        }
        if (d->data_len > 0) {
            if (s.rx_len + (size_t)d->data_len >= RX_MAX) {
                s.rx_overflow = true;
            } else {
                memcpy(s.rx + s.rx_len, d->data_ptr, (size_t)d->data_len);
                s.rx_len += (size_t)d->data_len;
            }
        }
        if (d->fin && d->payload_offset + d->data_len >= d->payload_len) {
            if (s.rx_overflow) {
                ESP_LOGW(TAG, "dropped oversized message");
            } else if (s.rx_len) {
                handle_message(s.rx, s.rx_len);
            }
            s.rx_len = 0;
            s.rx_overflow = false;
        }
        break;
    default:
        break;
    }
}

/* ---------- Uplink ---------- */

static void uplink_task(void *arg)
{
    (void)arg;
    EXT_RAM_BSS_ATTR static uint8_t pcm[UPLINK_CHUNK];   /* encoded to base64, never DMA */
    const size_t b64_cap = UPLINK_CHUNK / 3 * 4 + 8;
    const size_t json_cap = b64_cap + 64;
    char *json = heap_caps_malloc(json_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json) {
        ESP_LOGE(TAG, "uplink: no memory");
        vTaskDelete(NULL);
        return;
    }
    static const char prefix[] = "{\"type\":\"input_audio_buffer.append\",\"audio\":\"";
    for (;;) {
        if (!atomic_load(&s.uplink)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        size_t got = 0;
        while (got < UPLINK_CHUNK && atomic_load(&s.uplink)) {
            got += voice_audio_capture_read(pcm + got, UPLINK_CHUNK - got, 50);
        }
        if (!got || !atomic_load(&s.uplink)) {
            continue;
        }
        const size_t pl = sizeof(prefix) - 1;
        memcpy(json, prefix, pl);
        size_t b64_len = 0;
        mbedtls_base64_encode((unsigned char *)json + pl, json_cap - pl - 3, &b64_len, pcm, got);
        memcpy(json + pl + b64_len, "\"}", 3);
        if (!send_json(json)) {
            ESP_LOGW(TAG, "uplink send failed");
        }
    }
}

/* ---------- Control ---------- */

static void close_socket(void)
{
    atomic_store(&s.uplink, false);
    voice_audio_capture_end();
    if (s.ws) {
        esp_websocket_client_close(s.ws, pdMS_TO_TICKS(1000));
        esp_websocket_client_destroy(s.ws);
        s.ws = NULL;
    }
    s.ready = false;
}

static bool open_socket(void)
{
    if (s.ws) {
        return true;
    }
    if (esp_timer_get_time() < s.budget_lock_until_us) {
        ESP_LOGW(TAG, "daily voice budget used up; not connecting");
        app_buddy_set_status(s.buddy, "daily voice limit reached");
        return false;
    }
    if (!voice_wifi_wait_connected(3000)) {
        ESP_LOGW(TAG, "no Wi-Fi; can't reach Grok");
        app_buddy_set_status(s.buddy, "no Wi-Fi");
        app_buddy_express(s.buddy, APP_BUDDY_EXPR_SAD);
        return false;
    }
    esp_websocket_client_config_t cfg = {
        .uri = s.uri,
        .headers = s.auth_header,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 8192,
        .task_stack = 8192,
        .network_timeout_ms = 10000,
        .reconnect_timeout_ms = 10000,
        .disable_auto_reconnect = true,
        .ping_interval_sec = 15,
    };
    s.ws = esp_websocket_client_init(&cfg);
    if (!s.ws) {
        return false;
    }
    esp_websocket_register_events(s.ws, WEBSOCKET_EVENT_ANY, ws_handler, NULL);
    if (esp_websocket_client_start(s.ws) != ESP_OK) {
        esp_websocket_client_destroy(s.ws);
        s.ws = NULL;
        return false;
    }
    return true;
}

static cJSON *pcm_format(int rate)
{
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "type", "audio/pcm");
    cJSON_AddNumberToObject(f, "rate", rate);
    return f;
}

static void send_cjson(cJSON *o)
{
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (txt) {
        send_json(txt);
        cJSON_free(txt);
    }
}

static cJSON *add_enum(cJSON *props, const char *key, const char *desc, const char *const *vals, int n)
{
    cJSON *p = cJSON_AddObjectToObject(props, key);
    cJSON_AddStringToObject(p, "type", "string");
    cJSON_AddStringToObject(p, "description", desc);
    cJSON *e = cJSON_AddArrayToObject(p, "enum");
    for (int i = 0; i < n; ++i) {
        cJSON_AddItemToArray(e, cJSON_CreateString(vals[i]));
    }
    return p;
}

static void add_int(cJSON *props, const char *key, const char *desc)
{
    cJSON *p = cJSON_AddObjectToObject(props, key);
    cJSON_AddStringToObject(p, "type", "integer");
    cJSON_AddStringToObject(p, "description", desc);
}

/* Screen names Grok can ask for, matching app_voice_screen_t. */
static const char *const SCREEN_NAMES[] = { "timer", "workout", "eyes", "clock", "weather", "forecast", "usage", "calendar" };
#define SCREEN_COUNT (sizeof(SCREEN_NAMES) / sizeof(SCREEN_NAMES[0]))

static void tool_show_screen(const char *args, char *out, size_t len)
{
    cJSON *a = cJSON_Parse(args);
    const cJSON *sc = a ? cJSON_GetObjectItemCaseSensitive(a, "screen") : NULL;
    int which = -1;
    for (size_t i = 0; cJSON_IsString(sc) && i < SCREEN_COUNT; ++i) {
        if (!strcasecmp(sc->valuestring, SCREEN_NAMES[i])) {
            which = (int)i;
        }
    }
    if (which < 0) {
        snprintf(out, len, "{\"error\":\"unknown screen\"}");
    } else {
        show_screen((app_voice_screen_t)which);
        snprintf(out, len, "{\"showing\":\"%s\"}", SCREEN_NAMES[which]);
    }
    cJSON_Delete(a);
}

static void add_device_tools(cJSON *tools, bool camera)
{
    {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "function");
        cJSON_AddStringToObject(t, "name", "show_screen");
        cJSON_AddStringToObject(t, "description", "Switch the device display to one of its screens.");
        cJSON *params = cJSON_AddObjectToObject(t, "parameters");
        cJSON_AddStringToObject(params, "type", "object");
        add_enum(cJSON_AddObjectToObject(params, "properties"), "screen", "Which screen", SCREEN_NAMES,
                 (int)SCREEN_COUNT);
        cJSON_AddItemToArray(cJSON_AddArrayToObject(params, "required"), cJSON_CreateString("screen"));
        cJSON_AddItemToArray(tools, t);
    }
    {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "function");
        cJSON_AddStringToObject(t, "name", "timer_control");
        cJSON_AddStringToObject(t, "description", "Pause, resume, reset or read the countdown timer.");
        cJSON *params = cJSON_AddObjectToObject(t, "parameters");
        cJSON_AddStringToObject(params, "type", "object");
        static const char *const acts[] = { "pause", "resume", "reset", "status" };
        add_enum(cJSON_AddObjectToObject(params, "properties"), "action", "What to do", acts, 4);
        cJSON_AddItemToArray(cJSON_AddArrayToObject(params, "required"), cJSON_CreateString("action"));
        cJSON_AddItemToArray(tools, t);
    }
    {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "function");
        cJSON_AddStringToObject(t, "name", "workout");
        cJSON_AddStringToObject(t, "description", "Workout interval timer: start a preset or custom intervals, "
                                                  "pause, resume, stop, or report progress.");
        cJSON *params = cJSON_AddObjectToObject(t, "parameters");
        cJSON_AddStringToObject(params, "type", "object");
        cJSON *props = cJSON_AddObjectToObject(params, "properties");
        static const char *const acts[] = { "start", "pause", "resume", "stop", "status" };
        add_enum(props, "action", "What to do", acts, 5);
        static const char *const presets[] = { "tabata", "emom", "interval", "rounds", "stopwatch" };
        add_enum(props, "preset", "Preset to start (omit when giving custom times)", presets, 5);
        add_int(props, "work_seconds", "Custom: seconds of work per round (EMOM: 60)");
        add_int(props, "rest_seconds", "Custom: seconds of rest between rounds (0 = none)");
        add_int(props, "rounds", "Number of rounds/minutes (1-99)");
        cJSON_AddItemToArray(cJSON_AddArrayToObject(params, "required"), cJSON_CreateString("action"));
        cJSON_AddItemToArray(tools, t);
    }
    if (camera) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "function");
        cJSON_AddStringToObject(t, "name", "look");
        cJSON_AddStringToObject(t, "description", "Take one photo with the device camera and get a description "
                                                  "answering the question. Takes a few seconds.");
        cJSON *params = cJSON_AddObjectToObject(t, "parameters");
        cJSON_AddStringToObject(params, "type", "object");
        cJSON *q = cJSON_AddObjectToObject(cJSON_AddObjectToObject(params, "properties"), "question");
        cJSON_AddStringToObject(q, "type", "string");
        cJSON_AddStringToObject(q, "description", "What to find out from the photo, in the user's words");
        cJSON_AddItemToArray(cJSON_AddArrayToObject(params, "required"), cJSON_CreateString("question"));
        cJSON_AddItemToArray(tools, t);
    }
}

static void send_session_update(void)
{
    const bool camera = s.bot_enabled && app_camera_present();   /* look goes through the relay */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "session.update");
    cJSON *ss = cJSON_AddObjectToObject(o, "session");
    cJSON_AddStringToObject(ss, "voice", s.voice);
    cJSON_AddStringToObject(cJSON_AddObjectToObject(ss, "reasoning"), "effort", "none");   /* fastest turns */
    {
        char now[64];
        app_alarm_format_now(now, sizeof(now));
        const char *base = s.bot_enabled
                               ? INSTRUCTIONS BOT_INSTRUCTIONS ALARM_INSTRUCTIONS WORKOUT_INSTRUCTIONS SCREEN_INSTRUCTIONS
                               : INSTRUCTIONS ALARM_INSTRUCTIONS WORKOUT_INSTRUCTIONS SCREEN_INSTRUCTIONS;
        const char *look = camera ? LOOK_INSTRUCTIONS : "";
        const size_t n = strlen(base) + strlen(look) + strlen(now) + 48;
        char *ins = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ins) {
            snprintf(ins, n, "%s%s Current local date and time: %s.", base, look, now);
            cJSON_AddStringToObject(ss, "instructions", ins);
            heap_caps_free(ins);
        } else {
            cJSON_AddStringToObject(ss, "instructions", base);
        }
    }
    cJSON_AddStringToObject(cJSON_AddObjectToObject(ss, "turn_detection"), "type", "server_vad");
    cJSON *audio = cJSON_AddObjectToObject(ss, "audio");
    cJSON_AddItemToObject(cJSON_AddObjectToObject(audio, "input"), "format", pcm_format(VOICE_UPLINK_RATE));
    cJSON_AddItemToObject(cJSON_AddObjectToObject(audio, "output"), "format", pcm_format(VOICE_DOWNLINK_RATE));
    cJSON *tools = cJSON_AddArrayToObject(ss, "tools");
    {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "function");
        cJSON_AddStringToObject(t, "name", "set_alarm");
        cJSON_AddStringToObject(t, "description", "Set a one-shot alarm or timer on this device: either "
                                                  "`time` (next occurrence of a local clock time) or "
                                                  "`in_seconds` (relative).");
        cJSON *params = cJSON_AddObjectToObject(t, "parameters");
        cJSON_AddStringToObject(params, "type", "object");
        cJSON *props = cJSON_AddObjectToObject(params, "properties");
        cJSON *time_p = cJSON_AddObjectToObject(props, "time");
        cJSON_AddStringToObject(time_p, "type", "string");
        cJSON_AddStringToObject(time_p, "description", "Local clock time, 24-hour HH:MM, e.g. 06:30");
        cJSON *in_p = cJSON_AddObjectToObject(props, "in_seconds");
        cJSON_AddStringToObject(in_p, "type", "integer");
        cJSON_AddStringToObject(in_p, "description", "Ring this many seconds from now (relative alarms/timers)");
        cJSON *daily = cJSON_AddObjectToObject(props, "daily");
        cJSON_AddStringToObject(daily, "type", "boolean");
        cJSON_AddStringToObject(daily, "description", "With `time`: repeat every day (default false = ring once)");
        cJSON *label = cJSON_AddObjectToObject(props, "label");
        cJSON_AddStringToObject(label, "type", "string");
        cJSON_AddStringToObject(label, "description", "Optional short label, e.g. gym");
        cJSON_AddItemToArray(tools, t);
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "type", "function");
        cJSON_AddStringToObject(c, "name", "cancel_alarms");
        cJSON_AddStringToObject(c, "description", "Cancel all pending alarms on this device.");
        cJSON *cp = cJSON_AddObjectToObject(c, "parameters");
        cJSON_AddStringToObject(cp, "type", "object");
        cJSON_AddObjectToObject(cp, "properties");
        cJSON_AddItemToArray(tools, c);
    }
    add_device_tools(tools, camera);
    if (s.bot_enabled) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "function");
        cJSON_AddStringToObject(t, "name", "ask_grok_bot");
        cJSON_AddStringToObject(t, "description",
                                "Send a task to " OWNER "'s personal Grok Bot agent, " BOT_NAME
                                ", which has his accounts, files, memory and can act. The answer "
                                "arrives later and is read aloud.");
        cJSON *params = cJSON_AddObjectToObject(t, "parameters");
        cJSON_AddStringToObject(params, "type", "object");
        cJSON *props = cJSON_AddObjectToObject(params, "properties");
        cJSON *task = cJSON_AddObjectToObject(props, "task");
        cJSON_AddStringToObject(task, "type", "string");
        cJSON_AddStringToObject(task, "description", "Complete, self-contained instruction for " BOT_NAME);
        cJSON *req = cJSON_AddArrayToObject(params, "required");
        cJSON_AddItemToArray(req, cJSON_CreateString("task"));
        cJSON_AddItemToArray(tools, t);
    }
    send_cjson(o);
}

/* Return a tool result to Grok and let it respond. */
static void send_tool_output(const char *call_id, const char *output)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "conversation.item.create");
    cJSON *item = cJSON_AddObjectToObject(o, "item");
    cJSON_AddStringToObject(item, "type", "function_call_output");
    cJSON_AddStringToObject(item, "call_id", call_id);
    cJSON_AddStringToObject(item, "output", output);
    send_cjson(o);
    send_json("{\"type\":\"response.create\"}");
}

/* Ask Grok to read a bot answer aloud. Requires a ready session. */
static void speak_announcement(const char *text)
{
    const bool attention = text[0] == ATTN_MARK;
    const bool greeting = text[0] == GREET_MARK;
    const char *prefix = attention ? ATTENTION_PREFIX : greeting ? GREETING_PREFIX : ANNOUNCE_PREFIX;
    if (attention || greeting) {
        ++text;
    }
    const size_t n = strlen(prefix) + strlen(text) + 1;
    char *msg = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!msg) {
        return;
    }
    snprintf(msg, n, "%s%s", prefix, text);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "conversation.item.create");
    cJSON *item = cJSON_AddObjectToObject(o, "item");
    cJSON_AddStringToObject(item, "type", "message");
    cJSON_AddStringToObject(item, "role", "user");
    cJSON *content = cJSON_AddArrayToObject(item, "content");
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "type", "input_text");
    cJSON_AddStringToObject(part, "text", msg);
    cJSON_AddItemToArray(content, part);
    heap_caps_free(msg);
    send_cjson(o);
    if (!greeting) {
        voice_audio_chime();
    }
    if (attention) {
        voice_audio_chime();   /* double chime: something is waiting on you */
    }
    s.response_done = false;
    set_state(V_THINKING);
    send_json("{\"type\":\"response.create\"}");
}

static void announce_next_if_ready(void)
{
    if (!s.announce_count || s.asleep || s.alarm_ringing || night_active() || s.state != V_IDLE ||
            voice_audio_playing()) {
        return;
    }
    if (!s.ready) {
        /* EV_SESSION_READY retries; back off so a dead link doesn't spin. */
        static int64_t last_try_us;
        const int64_t now = esp_timer_get_time();
        if (!s.ws && now - last_try_us > 5LL * 1000 * 1000) {
            last_try_us = now;
            open_socket();
        }
        return;
    }
    char *text = s.announce[0];
    memmove(&s.announce[0], &s.announce[1], sizeof(s.announce[0]) * (MAX_ANNOUNCE - 1));
    s.announce[--s.announce_count] = NULL;
    ESP_LOGI(TAG, "announcing %s", text[0] == ATTN_MARK ? "bot attention notice"
                                 : text[0] == GREET_MARK ? "alarm greeting" : "bot answer");
    speak_announcement(text);
    heap_caps_free(text);
}

static const char *action_of(const cJSON *a)
{
    const cJSON *v = a ? cJSON_GetObjectItemCaseSensitive(a, "action") : NULL;
    return cJSON_IsString(v) ? v->valuestring : "";
}

static int int_of(const cJSON *a, const char *key, int dflt)
{
    const cJSON *v = a ? cJSON_GetObjectItemCaseSensitive(a, key) : NULL;
    return cJSON_IsNumber(v) ? (int)v->valuedouble : dflt;
}

static void tool_timer_control(const char *args, char *out, size_t len)
{
    cJSON *a = cJSON_Parse(args);
    const char *act = action_of(a);
    app_alarm_timer_t tm;
    const bool have = app_alarm_timer_get(&tm);
    if (!have) {
        snprintf(out, len, "{\"status\":\"no timer set\"}");
    } else if (!strcmp(act, "reset")) {
        if (tm.done) {
            app_alarm_dismiss();
        }
        app_alarm_timer_reset();
        snprintf(out, len, "{\"status\":\"reset\"}");
    } else if (!strcmp(act, "pause") || !strcmp(act, "resume")) {
        const bool want_run = !strcmp(act, "resume");
        if (!tm.done && tm.running != want_run) {
            app_alarm_timer_toggle();
        }
        snprintf(out, len, "{\"status\":\"%s\"}", want_run ? "running" : "paused");
    } else {
        const int left = (tm.remaining_ms + 999) / 1000;
        snprintf(out, len, "{\"remaining\":\"%d:%02d\",\"state\":\"%s\",\"label\":\"%s\"}", left / 60, left % 60,
                 tm.done ? "done" : tm.running ? "running" : "paused", tm.label);
    }
    cJSON_Delete(a);
}

static void describe_workout(char *out, size_t len, const char *status)
{
    app_workout_state_t st;
    app_workout_get(&st);
    static const char *const PH[] = { "not started", "get ready", "work", "rest", "done" };
    if (st.total_s == 0) {
        snprintf(out, len, "{\"status\":\"%s\",\"plan\":\"stopwatch\",\"phase\":\"%s\",\"elapsed_s\":%d}",
                 status, PH[st.phase], st.phase == APP_WORKOUT_WORK ? st.phase_left_ms / 1000 : 0);
        return;
    }
    snprintf(out, len, "{\"status\":\"%s\",\"plan\":\"%s %ds work/%ds rest x%d\",\"phase\":\"%s%s\","
             "\"round\":%d,\"seconds_left_in_phase\":%d,\"total_minutes\":%.1f}",
             status, st.name, st.plan.work_s, st.plan.rest_s, st.plan.rounds, PH[st.phase],
             st.paused ? " (paused)" : "", st.round, (st.phase_left_ms + 999) / 1000, st.total_s / 60.0);
}

static void tool_workout(const char *args, char *out, size_t len)
{
    cJSON *a = cJSON_Parse(args);
    const char *act = action_of(a);
    if (!strcmp(act, "start")) {
        const int work = int_of(a, "work_seconds", 0);
        const int rounds = int_of(a, "rounds", 0);
        const cJSON *pv = a ? cJSON_GetObjectItemCaseSensitive(a, "preset") : NULL;
        const int preset = app_workout_preset_from_name(cJSON_IsString(pv) ? pv->valuestring : NULL);
        bool ok = true;
        if (work > 0) {
            const app_workout_plan_t plan = { work, int_of(a, "rest_seconds", 0), rounds > 0 ? rounds : 1 };
            ok = app_workout_set_custom(&plan) == ESP_OK;
        } else {
            app_workout_state_t cur;
            app_workout_get(&cur);   /* no preset named: whatever is on the screen */
            app_workout_select(preset >= 0 ? (app_workout_preset_t)preset : cur.preset);
            if (rounds > 0) {
                app_workout_state_t st;
                app_workout_get(&st);
                app_workout_adjust_rounds(rounds - st.plan.rounds);
            }
        }
        if (!ok) {
            snprintf(out, len, "{\"status\":\"failed\",\"reason\":\"work 5 s to 99 min, rest up to 99 min, "
                               "1 to 99 rounds\"}");
        } else {
            app_workout_start();
            show_screen(APP_VOICE_SCREEN_WORKOUT);
            describe_workout(out, len, "started; 10 second get-ready now");
        }
    } else if (!strcmp(act, "pause")) {
        app_workout_pause();
        describe_workout(out, len, "paused");
    } else if (!strcmp(act, "resume")) {
        app_workout_resume();
        describe_workout(out, len, "resumed");
    } else if (!strcmp(act, "stop")) {
        app_workout_stop();
        snprintf(out, len, "{\"status\":\"stopped\"}");
    } else {
        describe_workout(out, len, "ok");
    }
    cJSON_Delete(a);
}

static void play_beep(app_voice_beep_t p)
{
    const int amp = 15000;
    switch (p) {
    case APP_VOICE_BEEP_COUNT:
        voice_audio_tone(880, 140, amp);
        break;
    case APP_VOICE_BEEP_START:
        voice_audio_tone(990, 120, amp);
        voice_audio_tone(0, 60, 0);
        voice_audio_tone(990, 120, amp);
        break;
    case APP_VOICE_BEEP_WORK:
        voice_audio_tone(1320, 650, amp);
        break;
    case APP_VOICE_BEEP_REST:
        voice_audio_tone(660, 260, amp);
        voice_audio_tone(0, 80, 0);
        voice_audio_tone(660, 260, amp);
        break;
    case APP_VOICE_BEEP_DONE:
        for (int i = 0; i < 3; ++i) {
            voice_audio_tone(1320, 160, amp);
            voice_audio_tone(0, 90, 0);
        }
        voice_audio_tone(1760, 700, amp);
        break;
    }
}

static void on_bot_event(voice_bot_event_t ev, const char *call_id, const char *text, void *ctx)
{
    (void)ctx;
    switch (ev) {
    case VOICE_BOT_SENT:
        post_msg(EV_TOOL_SENT, NULL, call_id);
        break;
    case VOICE_BOT_SEND_FAILED:
        post_msg(EV_TOOL_FAILED, NULL, call_id);
        break;
    case VOICE_BOT_RESULT:
        post_msg(EV_BOT_RESULT, text, NULL);
        break;
    case VOICE_BOT_ATTENTION:
        post_msg(EV_BOT_ATTENTION, text, NULL);
        break;
    case VOICE_BOT_LOOK_DONE:
        post_msg(EV_LOOK_DONE, text, call_id);
        break;
    }
}

static void start_listening(void)
{
    voice_audio_capture_begin();
    s.heard_speech = false;
    set_state(s.ready ? V_LISTENING : V_CONNECTING);
    if (s.ready) {
        atomic_store(&s.uplink, true);
    }
}

static void stop_turn_to_idle(void)
{
    atomic_store(&s.uplink, false);
    voice_audio_capture_end();
    set_state(V_IDLE);
}

static void on_event(const msg_t *m)
{
    const event_t ev = m->ev;
    switch (ev) {
    case EV_SLEEP:
        s.asleep = true;
        app_wake_set_enabled(false);
        if (s.state == V_SPEAKING || s.state == V_THINKING) {
            atomic_store(&s.discard, true);
            send_json("{\"type\":\"response.cancel\"}");
        }
        voice_audio_flush_playback();
        close_socket();
        stop_turn_to_idle();
        break;
    case EV_WAKE:
        s.asleep = false;
        update_wake_enable();
        s.last_activity_us = esp_timer_get_time();
        announce_next_if_ready();
        break;
    case EV_LOCAL_TOOL: {
        char out[320] = "";
        char *nl = m->text ? strchr(m->text, '\n') : NULL;
        if (nl) {
            *nl = '\0';
            if (!strcmp(m->text, "timer_control")) {
                tool_timer_control(nl + 1, out, sizeof(out));
            } else if (!strcmp(m->text, "workout")) {
                tool_workout(nl + 1, out, sizeof(out));
            } else if (!strcmp(m->text, "show_screen")) {
                tool_show_screen(nl + 1, out, sizeof(out));
            } else if (!strcmp(m->text, "cancel_alarms")) {
                snprintf(out, sizeof(out), "{\"cancelled\":%d}", app_alarm_cancel_all());
            } else {
                cJSON *a = cJSON_Parse(nl + 1);
                const cJSON *tm = a ? cJSON_GetObjectItemCaseSensitive(a, "time") : NULL;
                const cJSON *in = a ? cJSON_GetObjectItemCaseSensitive(a, "in_seconds") : NULL;
                const cJSON *lb = a ? cJSON_GetObjectItemCaseSensitive(a, "label") : NULL;
                const char *label = cJSON_IsString(lb) ? lb->valuestring : "";
                int hh = -1, mm = -1;
                char when[48] = "";
                const bool rel = cJSON_IsNumber(in);
                if (rel || (cJSON_IsString(tm) && sscanf(tm->valuestring, "%d:%d", &hh, &mm) == 2)) {
                    const cJSON *dy = a ? cJSON_GetObjectItemCaseSensitive(a, "daily") : NULL;
                    esp_err_t e;
                    const int secs = rel ? (int)in->valuedouble : 0;
                    if (rel && secs >= 1 && secs <= APP_ALARM_TIMER_MAX_S) {
                        /* Short relative ones run on the timer screen (works before clock sync). */
                        e = app_alarm_timer_set(secs, label, true);
                        snprintf(when, sizeof(when), "timer started: %d:%02d", secs / 60, secs % 60);
                        if (e == ESP_OK && !app_workout_active()) {
                            show_screen(APP_VOICE_SCREEN_TIMER);
                        }
                    } else if (rel) {
                        e = app_alarm_set_in(secs, label, when, sizeof(when));
                    } else {
                        /* Clock times go on the watch face's blue alarm hand. */
                        const bool every_day = cJSON_IsTrue(dy);
                        e = app_alarm_set_watch_ex(hh, mm, true, !every_day, label);
                        if (e == ESP_OK) {
                            bool tomorrow = false;
                            if (app_alarm_time_valid()) {
                                time_t nt = time(NULL);
                                struct tm lt;
                                localtime_r(&nt, &lt);
                                tomorrow = hh * 60 + mm < lt.tm_hour * 60 + lt.tm_min;
                            }
                            snprintf(when, sizeof(when), "%s %02d:%02d%s", every_day ? "every day at" :
                                     (tomorrow ? "tomorrow" : "today"), hh, mm, every_day ? "" : " (once)");
                        }
                    }
                    if (e == ESP_OK) {
                        snprintf(out, sizeof(out), "{\"status\":\"set\",\"rings\":\"%s\"}", when);
                    } else {
                        snprintf(out, sizeof(out), "{\"status\":\"failed\",\"reason\":\"%s\"}",
                                 e == ESP_ERR_INVALID_STATE ? "clock still syncing; try again in a few seconds" :
                                 e == ESP_ERR_NO_MEM ? "too many alarms (max 4)" : "bad time");
                    }
                } else {
                    snprintf(out, sizeof(out), "{\"status\":\"failed\",\"reason\":\"give time as HH:MM or in_seconds\"}");
                }
                cJSON_Delete(a);
            }
        }
        send_tool_output(m->call_id, out[0] ? out : "{\"status\":\"failed\"}");
        break;
    }
    case EV_ALARM_RING: {
        /* Morning alarm: ask the bot for a briefing now so it's ready soon after. */
        time_t now_t = time(NULL);
        struct tm lt;
        localtime_r(&now_t, &lt);
        if (s.bot_enabled && !app_alarm_ring_is_timer() && app_alarm_time_valid() && lt.tm_hour >= 4 &&
                lt.tm_hour < 12) {
            ESP_LOGI(TAG, "morning alarm: asking %s for a briefing", BOT_NAME);
            voice_bot_send("", BRIEFING_TASK);
        }
    }
        /* Stop any conversation; the alarm owns the speaker until dismissed. */
        if (s.state == V_SPEAKING || s.state == V_THINKING) {
            atomic_store(&s.discard, true);
            send_json("{\"type\":\"response.cancel\"}");
        }
        voice_audio_flush_playback();
        stop_turn_to_idle();
        s.alarm_ringing = true;
        s.next_chime_us = 0;
        break;
    case EV_ALARM_STOP:
        s.alarm_ringing = false;
        if (!app_alarm_ring_is_timer()) {
            s.night_lift_until_us = esp_timer_get_time() + NIGHT_LIFT_ALARM_US;   /* morning */
        }
        voice_audio_flush_playback();
        if (app_alarm_ring_is_timer()) {
            break;   /* a finished timer needs no greeting */
        }
        if (s.announce_count < MAX_ANNOUNCE) {
            const size_t n = (m->text ? strlen(m->text) : 0) + 2;
            char *t = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (t) {
                snprintf(t, n, "\x02%s", m->text ? m->text : "");
                s.announce[s.announce_count++] = t;
            }
        }
        announce_next_if_ready();
        break;
    case EV_WAKE_WORD:
        if (s.asleep || s.state != V_IDLE) {
            break;
        }
        app_buddy_react(s.buddy, APP_BUDDY_REACT_STARTLE);
        if (open_socket()) {
            start_listening();
        }
        break;
    case EV_PREFS:
        update_wake_enable();
        break;
    case EV_STOP:
        if (night_active()) {
            s.night_lift_until_us = esp_timer_get_time() + NIGHT_LIFT_TAP_US;
            atomic_store(&s.night_now, false);
        }
        if (s.state == V_SPEAKING || s.state == V_THINKING) {
            atomic_store(&s.discard, true);
            send_json("{\"type\":\"response.cancel\"}");
            voice_audio_flush_playback();
            stop_turn_to_idle();
        } else if (s.state == V_LISTENING || s.state == V_CONNECTING) {
            if (s.ready) {
                send_json("{\"type\":\"input_audio_buffer.clear\"}");
            }
            stop_turn_to_idle();
        }
        break;
    case EV_TAP:
        if (app_alarm_dismiss()) {
            break;   /* a tap silences a ringing alarm */
        }
        app_buddy_set_note(s.buddy, APP_BUDDY_NOTE_ALERT, "");   /* seen */
        if (night_active()) {
            s.night_lift_until_us = esp_timer_get_time() + NIGHT_LIFT_TAP_US;
            atomic_store(&s.night_now, false);
            announce_next_if_ready();
        }
        if (s.asleep) {
            break;
        }
        if (s.state == V_SPEAKING || s.state == V_THINKING) {
            /* Barge in: stop Grok and listen again. */
            atomic_store(&s.discard, true);
            send_json("{\"type\":\"response.cancel\"}");
            voice_audio_flush_playback();
            start_listening();
        } else if (s.state == V_LISTENING || s.state == V_CONNECTING) {
            /* Tap again to cancel listening. */
            if (s.ready) {
                send_json("{\"type\":\"input_audio_buffer.clear\"}");
            }
            stop_turn_to_idle();
        } else if (open_socket()) {
            start_listening();
        } else {
            set_state(V_THINKING);   /* brief puzzled look, then idle via timeout */
            s.state_since_us = esp_timer_get_time() - (int64_t)(THINK_MAX_MS - 1500) * 1000;
        }
        break;
    case EV_WS_CONNECTED:
        send_session_update();
        break;
    case EV_SESSION_READY:
        if (!s.ready) {
            ESP_LOGI(TAG, "session ready (voice %s)", s.voice);
        }
        s.ready = true;
        if (s.state == V_CONNECTING) {
            set_state(V_LISTENING);
            atomic_store(&s.uplink, true);   /* flushes audio captured while connecting */
        } else {
            announce_next_if_ready();
        }
        break;
    case EV_SPEECH_STARTED:
        s.heard_speech = true;
        s.last_activity_us = esp_timer_get_time();
        break;
    case EV_SPEECH_STOPPED:
        if (s.state == V_LISTENING) {
            atomic_store(&s.uplink, false);
            voice_audio_capture_end();
            s.response_done = false;
            set_state(V_THINKING);
        }
        break;
    case EV_RESPONSE_CREATED:
        s.response_done = false;
        break;
    case EV_AUDIO:
        /* IDLE too: a reply to a tool result can start after a quiet gap. */
        if (s.state == V_THINKING || s.state == V_IDLE) {
            set_state(V_SPEAKING);
        }
        s.last_activity_us = esp_timer_get_time();
        break;
    case EV_RESPONSE_DONE:
        s.response_done = true;
        if (s.state == V_THINKING && !s.tool_pending && !voice_audio_playing()) {
            set_state(V_IDLE);   /* response without audio */
        }
        break;
    case EV_BUDGET:
        /* Re-check after 30 min; the relay resets the budget at local midnight. */
        s.budget_lock_until_us = esp_timer_get_time() + 30LL * 60 * 1000 * 1000;
        atomic_store(&s.uplink, false);
        voice_audio_capture_end();
        set_state(V_IDLE);
        break;
    case EV_ERROR:
        if (s.state == V_CONNECTING || s.state == V_THINKING) {
            stop_turn_to_idle();
        }
        break;
    case EV_TOOL_CALL:
        s.tool_pending = true;
        if (s.state != V_SPEAKING) {
            set_state(V_THINKING);
        }
        if (!voice_bot_send(m->call_id, m->text)) {
            s.tool_pending = false;
            send_tool_output(m->call_id, "{\"status\":\"failed\",\"reason\":\"busy\"}");
        }
        break;
    case EV_TOOL_SENT:
        s.pending_tasks++;
        s.pending_since_us = esp_timer_get_time();
        app_buddy_set_busy(s.buddy, true);
        if (!m->call_id[0]) {
            break;   /* device-initiated task (morning briefing): no tool call to answer */
        }
        s.tool_pending = false;
        send_tool_output(m->call_id,
                         "{\"status\":\"sent\",\"note\":\"" BOT_NAME " is working on it; its answer "
                         "will be read aloud when it arrives.\"}");
        break;
    case EV_TOOL_FAILED:
        if (!m->call_id[0]) {
            ESP_LOGW(TAG, "couldn't send the morning briefing request");
            break;
        }
        s.tool_pending = false;
        send_tool_output(m->call_id, "{\"status\":\"failed\",\"reason\":\"couldn't reach " BOT_NAME "\"}");
        break;
    case EV_BEEP:
        play_beep((app_voice_beep_t)(m->call_id[0] - '0'));
        break;
    case EV_NIGHT:
        s.night = m->call_id[0] == '1';
        if (!s.night) {
            s.night_lift_until_us = 0;
        }
        ESP_LOGI(TAG, "night mode %s", s.night ? "on" : "off");
        update_wake_enable();
        announce_next_if_ready();
        break;
    case EV_LOOK:
        if (!s.bot_enabled || !voice_bot_look(m->call_id, m->text)) {
            send_tool_output(m->call_id, "{\"status\":\"failed\",\"reason\":\"camera unavailable\"}");
            break;
        }
        app_buddy_express(s.buddy, APP_BUDDY_EXPR_CURIOUS);
        app_buddy_set_status(s.buddy, "looking...");
        break;
    case EV_LOOK_DONE:
        app_buddy_set_status(s.buddy, "");
        if (m->call_id[0]) {
            send_tool_output(m->call_id, m->text ? m->text : "{\"status\":\"failed\"}");
        }
        break;
    case EV_BOT_RESULT:
    case EV_BOT_ATTENTION:
        if (ev == EV_BOT_ATTENTION) {
            app_buddy_set_note(s.buddy, APP_BUDDY_NOTE_ALERT, BOT_NAME " needs you");
        }
        if (ev == EV_BOT_RESULT && s.pending_tasks > 0 && --s.pending_tasks == 0) {
            app_buddy_set_busy(s.buddy, false);
        }
        if (s.announce_count < MAX_ANNOUNCE && m->text) {
            const size_t n = strlen(m->text) + 2;
            char *t = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (t) {
                snprintf(t, n, "%s%s", ev == EV_BOT_ATTENTION ? "\x01" : "", m->text);
                s.announce[s.announce_count++] = t;
            }
        } else {
            ESP_LOGW(TAG, "dropping bot message; announcement queue full");
        }
        announce_next_if_ready();
        break;
    case EV_WS_CLOSED:
        if (s.ws) {
            ESP_LOGW(TAG, "realtime socket closed in state %s", STATE_NAMES[s.state]);
            close_socket();
        }
        if (s.state != V_SPEAKING) {
            stop_turn_to_idle();
        }
        break;
    }
}

static void on_tick(void)
{
    const int64_t now = esp_timer_get_time();
    atomic_store(&s.night_now, night_active());
    update_wake_enable();   /* playback ending or budget lock expiring re-arms it */
    if (s.alarm_ringing && now >= s.next_chime_us) {
        voice_audio_chime();
        voice_audio_chime();
        s.next_chime_us = now + 1500LL * 1000;
    }
    if (s.pending_tasks && (now - s.pending_since_us) / 1000 > PENDING_MAX_MS) {
        ESP_LOGW(TAG, "no answer from %s after 20 min; clearing busy", BOT_NAME);
        s.pending_tasks = 0;
        app_buddy_set_busy(s.buddy, false);
    }
    const int64_t in_state_ms = (now - s.state_since_us) / 1000;
    switch (s.state) {
    case V_CONNECTING:
        if (in_state_ms > CONNECT_MAX_MS) {
            ESP_LOGW(TAG, "connect timed out");
            close_socket();
            stop_turn_to_idle();
        }
        break;
    case V_LISTENING:
        if ((!s.heard_speech && in_state_ms > LISTEN_NO_SPEECH_MS) || in_state_ms > LISTEN_MAX_MS) {
            ESP_LOGI(TAG, "listening timed out");
            send_json("{\"type\":\"input_audio_buffer.clear\"}");
            stop_turn_to_idle();
        }
        break;
    case V_THINKING:
        if (in_state_ms > THINK_MAX_MS) {
            ESP_LOGW(TAG, "no reply; giving up");
            set_state(V_IDLE);
        }
        break;
    case V_SPEAKING:
        if (s.response_done && !voice_audio_playing()) {
            set_state(V_IDLE);
        }
        break;
    case V_IDLE:
        announce_next_if_ready();
        if (s.ws && !s.announce_count && (now - s.last_activity_us) / 1000 > IDLE_CLOSE_MS) {
            ESP_LOGI(TAG, "closing idle realtime socket");
            close_socket();
        }
        break;
    }
}

static void control_task(void *arg)
{
    (void)arg;
    for (;;) {
        msg_t m;
        if (xQueueReceive(s.q, &m, pdMS_TO_TICKS(100)) == pdTRUE) {
            on_event(&m);
            heap_caps_free(m.text);
        }
        on_tick();
    }
}

/* Build-time secrets from secrets/ (see CMakeLists.txt); each may be empty. */
extern const char xai_key_start[] asm("_binary_xai_api_key_start");
extern const char xai_key_end[] asm("_binary_xai_api_key_end");
extern const char wifi2_ssid_start[] asm("_binary_wifi2_ssid_start");
extern const char wifi2_ssid_end[] asm("_binary_wifi2_ssid_end");
extern const char wifi2_password_start[] asm("_binary_wifi2_password_start");
extern const char wifi2_password_end[] asm("_binary_wifi2_password_end");
extern const char relay_url_start[] asm("_binary_relay_url_start");
extern const char relay_url_end[] asm("_binary_relay_url_end");
extern const char relay_token_start[] asm("_binary_relay_token_start");
extern const char relay_token_end[] asm("_binary_relay_token_end");

static bool load_embedded(const char *start, const char *end, char *out, size_t cap)
{
    const char *p = start;
    size_t n = (size_t)(end - start);
    /* TEXT embedding appends a NUL; trim it and surrounding whitespace. */
    while (n && (p[n - 1] == '\0' || p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' ')) {
        --n;
    }
    while (n && (*p == ' ' || *p == '\n' || *p == '\r')) {
        ++p;
        --n;
    }
    if (!n || n >= cap) {
        return false;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

void app_voice_set_asleep(bool asleep)
{
    post(asleep ? EV_SLEEP : EV_WAKE);
}

void app_voice_set_alarm(bool ringing, const char *label)
{
    post_msg(ringing ? EV_ALARM_RING : EV_ALARM_STOP, label, NULL);
}

esp_err_t app_voice_start(const app_voice_config_t *config)
{
    if (!config || !config->buddy) {
        return ESP_ERR_INVALID_ARG;
    }
    s.buddy = config->buddy;

    /* Preferred: voice through the relay, which holds the xAI key (none on the board).
     * Fallback: connect to xAI directly with a key from Vibe Mode or secrets/. */
    char relay_url[128];
    char relay_token[96] = { 0 };
    const bool relay = load_embedded(relay_url_start, relay_url_end, relay_url, sizeof(relay_url)) &&
                       load_embedded(relay_token_start, relay_token_end, relay_token, sizeof(relay_token));
    esp_err_t err = ESP_OK;
    if (relay) {
        const char *host = strstr(relay_url, "://");
        snprintf(s.uri, sizeof(s.uri), "wss://%s/voice", host ? host + 3 : relay_url);
        snprintf(s.auth_header, sizeof(s.auth_header), "Authorization: Bearer %s\r\n", relay_token);
        s.via_relay = true;
        ESP_LOGI(TAG, "voice via relay (no API key on the device)");
    } else {
        char key[API_KEY_MAX];
        err = app_config_load_secret(APP_VOICE_KEY_API_KEY, key, sizeof(key));
        if (err != ESP_OK && !load_embedded(xai_key_start, xai_key_end, key, sizeof(key))) {
            ESP_LOGW(TAG, "no relay and no xAI key ('%s' in Vibe Mode or secrets/); voice disabled",
                     APP_VOICE_KEY_API_KEY);
            return ESP_ERR_NOT_FOUND;
        }
        snprintf(s.uri, sizeof(s.uri), "%s", REALTIME_URI);
        snprintf(s.auth_header, sizeof(s.auth_header), "Authorization: Bearer %s\r\n", key);
        mbedtls_platform_zeroize(key, sizeof(key));
        ESP_LOGI(TAG, "voice direct to xAI (key on the device)");
    }
    if (app_config_load_secret(APP_VOICE_KEY_VOICE, s.voice, sizeof(s.voice)) != ESP_OK) {
        strcpy(s.voice, DEFAULT_VOICE);
    }

    app_config_wifi_t wifi = { 0 };
    char ssid2[33] = "", pass2[65] = "";
    load_embedded(wifi2_ssid_start, wifi2_ssid_end, ssid2, sizeof(ssid2));
    load_embedded(wifi2_password_start, wifi2_password_end, pass2, sizeof(pass2));
    err = app_config_load_wifi(&wifi);
    if (err != ESP_OK && !ssid2[0]) {
        ESP_LOGW(TAG, "Wi-Fi not configured in Vibe Mode (%s); voice disabled", esp_err_to_name(err));
        mbedtls_platform_zeroize(s.auth_header, sizeof(s.auth_header));
        return ESP_ERR_NOT_FOUND;
    }
    err = voice_wifi_start(wifi.ssid, wifi.password, ssid2, pass2);
    mbedtls_platform_zeroize(&wifi, sizeof(wifi));
    mbedtls_platform_zeroize(pass2, sizeof(pass2));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
        return err;
    }

    s.rx = heap_caps_malloc(RX_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.q = xQueueCreate(32, sizeof(msg_t));
    if (!s.rx || !s.q) {
        return ESP_ERR_NO_MEM;
    }
    err = voice_audio_start(on_level, NULL);
    if (err != ESP_OK) {
        return err;
    }
    s.state = V_IDLE;
    s.state_since_us = s.last_activity_us = esp_timer_get_time();
    if (xTaskCreateWithCaps(control_task, "voice_ctl", 6144, NULL, 5, NULL, MALLOC_CAP_SPIRAM) != pdPASS ||
            xTaskCreateWithCaps(uplink_task, "voice_up", 6144, NULL, 5, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (relay) {
        s.bot_enabled = voice_bot_start(relay_url, relay_token, on_bot_event, NULL) == ESP_OK;
    }
    mbedtls_platform_zeroize(relay_token, sizeof(relay_token));
    ESP_LOGI(TAG, "%s hand-off %s", BOT_NAME, s.bot_enabled ? "enabled" : "not configured");
    app_buddy_set_tap_handler(s.buddy, on_tap, NULL);
    voice_audio_set_ambient_cb(on_ambient, NULL);
    const bool wake = app_wake_start(on_wake_word, NULL) == ESP_OK;
    update_wake_enable();
    ESP_LOGI(TAG, "Grok voice ready: tap to talk%s", wake ? " or say \"Hey Grok\"" : "");
    return ESP_OK;
}

void app_voice_set_summary_handler(app_voice_summary_cb_t cb, void *ctx)
{
    voice_bot_set_summary_cb(cb, ctx);
}

void app_voice_request_summary(void)
{
    voice_bot_request_summary();
}

void app_voice_beep(app_voice_beep_t pattern)
{
    char id[2] = { (char)('0' + (int)pattern), '\0' };
    post_msg(EV_BEEP, NULL, id);
}

void app_voice_set_night(bool night)
{
    if (night != s.night) {
        post_msg(EV_NIGHT, NULL, night ? "1" : "0");
    }
}

bool app_voice_night_active(void)
{
    return atomic_load(&s.night_now);
}

void app_voice_set_screen_handler(app_voice_screen_cb_t cb, void *ctx)
{
    s.screen_ctx = ctx;
    s.screen_cb = cb;
}

void app_voice_set_weather_handler(app_voice_summary_cb_t cb, void *ctx)
{
    voice_bot_set_weather_cb(cb, ctx);
}

void app_voice_set_calendar_handler(app_voice_summary_cb_t cb, void *ctx)
{
    voice_bot_set_calendar_cb(cb, ctx);
}

void app_voice_request_calendar(void)
{
    voice_bot_request_calendar();
}

void app_voice_request_weather(void)
{
    voice_bot_request_weather();
}
