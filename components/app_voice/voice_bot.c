// SPDX-License-Identifier: Apache-2.0
/*
 * Grok Bot hand-off through the grok-buddy-relay Worker.
 * One task owns all relay HTTP: it sends queued tasks (POST /task) and polls
 * for answers (GET /results) - every few seconds while a task is outstanding,
 * otherwise once a minute. Results are reported through the callback.
 */
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "voice_internal.h"
#include "app_camera.h"

static const char *TAG = "voice_bot";

#define URL_MAX        160
#define AUTH_MAX       96
#define RESP_MAX       (16 * 1024)
#define POLL_ACTIVE_MS 4000
#define POLL_IDLE_MS   60000
#define OUTSTANDING_MS (20 * 60 * 1000)   /* stop fast polling after 20 min */
#define HTTP_TIMEOUT_MS 15000

typedef struct {
    char call_id[VOICE_CALL_ID_MAX];
    char *task;          /* task text, or the question for a look */
    bool look;
} job_t;

static struct {
    char base[URL_MAX];
    char auth[AUTH_MAX];
    QueueHandle_t q;
    voice_bot_cb_t cb;
    void *ctx;
    int64_t outstanding_until_us;
    char *resp;
    size_t resp_len;
    volatile bool summary_wanted;
    voice_summary_cb_t summary_cb;
    void *summary_ctx;
    volatile bool calendar_wanted;
    voice_summary_cb_t calendar_cb;
    void *calendar_ctx;
    volatile bool weather_wanted;
    voice_summary_cb_t weather_cb;
    void *weather_ctx;
} b;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0) {
        const size_t room = RESP_MAX - 1 - b.resp_len;
        const size_t n = (size_t)e->data_len < room ? (size_t)e->data_len : room;
        memcpy(b.resp + b.resp_len, e->data, n);
        b.resp_len += n;
    }
    return ESP_OK;
}

/* Perform one request; returns HTTP status (0 on transport failure). */
static int request(esp_http_client_method_t method, const char *path, const char *body)
{
    char url[URL_MAX + 16];
    snprintf(url, sizeof(url), "%s%s", b.base, path);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_http,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return 0;
    }
    esp_http_client_set_header(c, "Authorization", b.auth);
    if (body) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, body, (int)strlen(body));
    }
    b.resp_len = 0;
    const esp_err_t err = esp_http_client_perform(c);
    const int status = err == ESP_OK ? esp_http_client_get_status_code(c) : 0;
    esp_http_client_cleanup(c);
    b.resp[b.resp_len] = '\0';
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s failed: %s", path, esp_err_to_name(err));
    }
    return status;
}

static void send_job(job_t *j)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "task", j->task);
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    const int status = body ? request(HTTP_METHOD_POST, "/task", body) : 0;
    cJSON_free(body);
    const bool ok = status >= 200 && status < 300;
    ESP_LOGI(TAG, "task sent: HTTP %d %s", status, ok ? "" : b.resp);
    if (ok) {
        b.outstanding_until_us = esp_timer_get_time() + (int64_t)OUTSTANDING_MS * 1000;
    }
    b.cb(ok ? VOICE_BOT_SENT : VOICE_BOT_SEND_FAILED, j->call_id, NULL, b.ctx);
}

/* Photo -> relay /look (vision model) -> tool output JSON for Grok. */
static void do_look(job_t *j)
{
    char out[1200];
    uint8_t *jpg = NULL;
    size_t len = 0;
    const esp_err_t cam = app_camera_capture_jpeg(&jpg, &len);
    if (cam != ESP_OK) {
        snprintf(out, sizeof(out), "{\"status\":\"failed\",\"reason\":\"%s\"}",
                 cam == ESP_ERR_NOT_FOUND ? "no camera is plugged in" : "the camera didn't take a picture");
        b.cb(VOICE_BOT_LOOK_DONE, j->call_id, out, b.ctx);
        return;
    }
    /* The question travels in a header: printable ASCII only, short. */
    char q[300];
    size_t k = 0;
    for (const char *p = j->task ? j->task : ""; *p && k < sizeof(q) - 1; ++p) {
        q[k++] = (*p >= 32 && *p < 127) ? *p : ' ';
    }
    q[k] = '\0';
    char url[URL_MAX + 16];
    snprintf(url, sizeof(url), "%s/look", b.base);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_http,
        .disable_auto_redirect = true,
    };
    int status = 0;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c) {
        esp_http_client_set_header(c, "Authorization", b.auth);
        esp_http_client_set_header(c, "Content-Type", "image/jpeg");
        esp_http_client_set_header(c, "X-Question", q);
        esp_http_client_set_post_field(c, (const char *)jpg, (int)len);
        b.resp_len = 0;
        const esp_err_t err = esp_http_client_perform(c);
        status = err == ESP_OK ? esp_http_client_get_status_code(c) : 0;
        esp_http_client_cleanup(c);
        b.resp[b.resp_len] = '\0';
    }
    heap_caps_free(jpg);
    ESP_LOGI(TAG, "look: %u byte photo, HTTP %d", (unsigned)len, status);
    cJSON *root = status == 200 ? cJSON_Parse(b.resp) : NULL;
    const cJSON *ans = root ? cJSON_GetObjectItemCaseSensitive(root, "answer") : NULL;
    cJSON *o = cJSON_CreateObject();
    if (cJSON_IsString(ans)) {
        cJSON_AddStringToObject(o, "status", "ok");
        cJSON_AddStringToObject(o, "what_the_camera_sees", ans->valuestring);
    } else {
        cJSON_AddStringToObject(o, "status", "failed");
        cJSON_AddStringToObject(o, "reason", status == 402 ? "Grok credits are used up" :
                                status == 429 ? "too many looks today" : "couldn't reach the vision model");
    }
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    cJSON_Delete(root);
    snprintf(out, sizeof(out), "%s", txt ? txt : "{\"status\":\"failed\"}");
    cJSON_free(txt);
    b.cb(VOICE_BOT_LOOK_DONE, j->call_id, out, b.ctx);
}

static void poll_results(void)
{
    const int status = request(HTTP_METHOD_GET, "/results", NULL);
    if (status != 200) {
        return;
    }
    cJSON *root = cJSON_Parse(b.resp);
    const cJSON *arr = root ? cJSON_GetObjectItemCaseSensitive(root, "results") : NULL;
    const cJSON *r;
    cJSON_ArrayForEach(r, arr) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(r, "text");
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(r, "status");
        const char *status = cJSON_IsString(st) ? st->valuestring : "done";
        if (!strcmp(status, "working") || !cJSON_IsString(text) || !text->valuestring[0]) {
            continue;
        }
        const bool attention = !strcmp(status, "attention");
        ESP_LOGI(TAG, "bot %s (%u chars)", attention ? "needs attention" : "answered",
                 (unsigned)strlen(text->valuestring));
        b.cb(attention ? VOICE_BOT_ATTENTION : VOICE_BOT_RESULT, "", text->valuestring, b.ctx);
    }
    cJSON_Delete(root);
}

static void fetch_summary(void)
{
    b.summary_wanted = false;
    const int status = request(HTTP_METHOD_GET, "/summary", NULL);
    if (b.summary_cb) {
        b.summary_cb(status == 200 ? b.resp : NULL, b.summary_ctx);
    }
}

static void fetch_weather(void)
{
    b.weather_wanted = false;
    const int status = request(HTTP_METHOD_GET, "/weather", NULL);
    if (b.weather_cb) {
        b.weather_cb(status == 200 ? b.resp : NULL, b.weather_ctx);
    }
}

static void fetch_calendar(void)
{
    b.calendar_wanted = false;
    const int status = request(HTTP_METHOD_GET, "/calendar", NULL);
    if (b.calendar_cb) {
        b.calendar_cb(status == 200 ? b.resp : NULL, b.calendar_ctx);
    }
}

static void bot_task(void *arg)
{
    (void)arg;
    int64_t next_poll_us = esp_timer_get_time() + 10LL * 1000 * 1000;
    for (;;) {
        job_t j;
        if (xQueueReceive(b.q, &j, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (j.look) {
                if (voice_wifi_wait_connected(5000)) {
                    do_look(&j);
                } else {
                    b.cb(VOICE_BOT_LOOK_DONE, j.call_id, "{\"status\":\"failed\",\"reason\":\"no Wi-Fi\"}", b.ctx);
                }
                heap_caps_free(j.task);
                continue;
            }
            if (voice_wifi_wait_connected(5000)) {
                send_job(&j);
            } else {
                b.cb(VOICE_BOT_SEND_FAILED, j.call_id, NULL, b.ctx);
            }
            heap_caps_free(j.task);
            next_poll_us = esp_timer_get_time() + (int64_t)POLL_ACTIVE_MS * 1000;
            continue;
        }
        if (b.summary_wanted && voice_wifi_wait_connected(0)) {
            fetch_summary();
        }
        if (b.weather_wanted && voice_wifi_wait_connected(0)) {
            fetch_weather();
        }
        if (b.calendar_wanted && voice_wifi_wait_connected(0)) {
            fetch_calendar();
        }
        const int64_t now = esp_timer_get_time();
        if (now >= next_poll_us) {
            if (voice_wifi_wait_connected(0)) {
                poll_results();
            }
            const bool active = now < b.outstanding_until_us;
            next_poll_us = esp_timer_get_time() + (int64_t)(active ? POLL_ACTIVE_MS : POLL_IDLE_MS) * 1000;
        }
    }
}

esp_err_t voice_bot_start(const char *base_url, const char *token, voice_bot_cb_t cb, void *ctx)
{
    if (!base_url || !token || !cb || strlen(base_url) >= URL_MAX || strlen(token) + 8 >= AUTH_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(b.base, sizeof(b.base), "%s", base_url);
    size_t n = strlen(b.base);
    while (n && b.base[n - 1] == '/') {
        b.base[--n] = '\0';
    }
    snprintf(b.auth, sizeof(b.auth), "Bearer %s", token);
    b.cb = cb;
    b.ctx = ctx;
    b.resp = heap_caps_malloc(RESP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    b.q = xQueueCreate(4, sizeof(job_t));
    if (!b.resp || !b.q) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreateWithCaps(bot_task, "voice_bot", 12288, NULL, 4, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Grok Bot relay ready");
    return ESP_OK;
}

bool voice_bot_send(const char *call_id, const char *task)
{
    if (!b.q || !task) {
        return false;
    }
    job_t j = { 0 };
    snprintf(j.call_id, sizeof(j.call_id), "%s", call_id ? call_id : "");
    const size_t n = strlen(task) + 1;
    j.task = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!j.task) {
        return false;
    }
    memcpy(j.task, task, n);
    if (xQueueSend(b.q, &j, 0) != pdTRUE) {
        heap_caps_free(j.task);
        return false;
    }
    return true;
}

bool voice_bot_look(const char *call_id, const char *question)
{
    if (!b.q) {
        return false;
    }
    job_t j = { .look = true };
    snprintf(j.call_id, sizeof(j.call_id), "%s", call_id ? call_id : "");
    const char *qs = question ? question : "";
    const size_t n = strlen(qs) + 1;
    j.task = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!j.task) {
        return false;
    }
    memcpy(j.task, qs, n);
    if (xQueueSend(b.q, &j, 0) != pdTRUE) {
        heap_caps_free(j.task);
        return false;
    }
    return true;
}

void voice_bot_set_summary_cb(voice_summary_cb_t cb, void *ctx)
{
    b.summary_ctx = ctx;
    b.summary_cb = cb;
}

void voice_bot_request_summary(void)
{
    b.summary_wanted = true;
}

void voice_bot_set_weather_cb(voice_summary_cb_t cb, void *ctx)
{
    b.weather_ctx = ctx;
    b.weather_cb = cb;
}

void voice_bot_set_calendar_cb(voice_summary_cb_t cb, void *ctx)
{
    b.calendar_ctx = ctx;
    b.calendar_cb = cb;
}

void voice_bot_request_calendar(void)
{
    b.calendar_wanted = true;
}

void voice_bot_request_weather(void)
{
    b.weather_wanted = true;
}
