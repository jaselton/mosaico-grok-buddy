// SPDX-License-Identifier: Apache-2.0
#include "app_camera.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "linux/videodev2.h"
#include "mosaico_module_camera.h"

static const char *TAG = "app_camera";

#define TARGET_LONG_SIDE 640
#define BUF_ALIGN        128
#define JPEG_MAX         (200 * 1024)
#define JPEG_QUALITY     75
#define SETTLE_FRAMES    4     /* let auto-exposure settle after power-up */
#define FRAME_TIMEOUT_MS 2500

static struct {
    bool ready;
    volatile bool present;
    SemaphoreHandle_t lock;
    ppa_client_handle_t ppa;
} c;

/* Look at the left slot once; the module manager (two tasks with internal-RAM
 * stacks and a 250 ms scan) runs only for this and during a capture. */
static bool identify(void)
{
    if (mosaico_module_mgr_init(NULL) != ESP_OK) {
        return false;
    }
    bool found = false;
    for (int i = 0; i < 20; ++i) {   /* debounce + descriptor read take ~1 s */
        mosaico_module_mgr_info_t info;
        if (mosaico_module_mgr_get_info(MOSAICO_MODULE_MGR_SLOT_LEFT, &info) == ESP_OK) {
            if (info.presence == MOSAICO_MODULE_PRESENCE_ABSENT) {
                break;
            }
            if (info.presence == MOSAICO_MODULE_PRESENCE_PRESENT &&
                    info.descriptor_state == MOSAICO_MODULE_DESCRIPTOR_VALID) {
                found = info.eeprom.board_type == MOSAICO_BOARD_TYPE_CAMERA;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    mosaico_module_mgr_deinit();
    return found;
}

esp_err_t app_camera_init(void)
{
    if (c.ready) {
        return ESP_OK;
    }
    c.lock = xSemaphoreCreateMutex();
    if (!c.lock) {
        return ESP_ERR_NO_MEM;
    }
    c.ready = true;
    c.present = identify();
    ESP_LOGI(TAG, "%s", c.present ? "camera module found (left slot)" : "no camera module; look is off");
    return ESP_OK;
}

bool app_camera_present(void)
{
    return c.ready && c.present;
}

static esp_err_t convert(const mosaico_camera_frame_t *fr, uint16_t *out, size_t out_size, int *ow, int *oh)
{
    if (fr->pixel_format != V4L2_PIX_FMT_UYVY) {
        ESP_LOGE(TAG, "unexpected pixel format 0x%08" PRIx32, fr->pixel_format);
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint32_t bpl = fr->bytes_per_line ? fr->bytes_per_line : fr->width * 2U;
    const uint32_t longer = fr->width > fr->height ? fr->width : fr->height;
    /* PPA scales in 1/16 steps: round down so the output fits. */
    float scale = floorf((float)TARGET_LONG_SIDE / (float)longer * 16.0f) / 16.0f;
    if (scale <= 0) {
        scale = 1.0f / 16.0f;
    }
    if (scale > 1.0f) {
        scale = 1.0f;
    }
    const int sw = ((int)(fr->width * scale)) & ~1;
    const int sh = ((int)(fr->height * scale)) & ~1;
    /* Rotated a quarter turn (the sensor sits sideways in the slot). */
    *ow = sh;
    *oh = sw;
    if ((size_t)(*ow) * (*oh) * 2 > out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_cache_msync((void *)fr->data, fr->size, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    esp_cache_msync(out, out_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    const ppa_srm_oper_config_t op = {
        .in = {
            .buffer = fr->data,
            .pic_w = bpl / 2U,
            .pic_h = fr->height,
            .block_w = fr->width,
            .block_h = fr->height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_YUV422_UYVY,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = out,
            .buffer_size = out_size,
            .pic_w = (uint32_t)*ow,
            .pic_h = (uint32_t)*oh,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
        .scale_x = scale,
        .scale_y = scale,
        .mirror_x = false,
        .mirror_y = true,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(c.ppa, &op);
    if (err == ESP_OK) {
        esp_cache_msync(out, out_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    }
    return err;
}

static esp_err_t encode(const uint16_t *rgb, int w, int h, uint8_t **jpeg, size_t *len)
{
    uint8_t *buf = heap_caps_malloc(JPEG_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = w;
    cfg.height = h;
    cfg.src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    cfg.subsampling = JPEG_SUBSAMPLE_420;
    cfg.quality = JPEG_QUALITY;
    jpeg_enc_handle_t enc = NULL;
    int out = 0;
    jpeg_error_t jr = jpeg_enc_open(&cfg, &enc);
    if (jr == JPEG_ERR_OK) {
        jr = jpeg_enc_process(enc, (const uint8_t *)rgb, w * h * 2, buf, JPEG_MAX, &out);
        jpeg_enc_close(enc);
    }
    if (jr != JPEG_ERR_OK || out <= 0) {
        heap_caps_free(buf);
        ESP_LOGE(TAG, "JPEG encode failed: %d", jr);
        return ESP_FAIL;
    }
    *jpeg = buf;
    *len = (size_t)out;
    return ESP_OK;
}

esp_err_t app_camera_capture_jpeg(uint8_t **jpeg, size_t *len)
{
    *jpeg = NULL;
    *len = 0;
    if (!c.ready) {
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(c.lock, portMAX_DELAY);
    const ppa_client_config_t pcfg = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    if (ppa_register_client(&pcfg, &c.ppa) != ESP_OK) {
        xSemaphoreGive(c.lock);
        return ESP_ERR_NO_MEM;
    }
    const size_t out_size = (TARGET_LONG_SIDE * TARGET_LONG_SIDE * 2 + BUF_ALIGN - 1) & ~(size_t)(BUF_ALIGN - 1);
    uint16_t *rgb = heap_caps_aligned_calloc(BUF_ALIGN, 1, out_size,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    mosaico_camera_handle_t cam = NULL;
    mosaico_camera_config_t cfg = MOSAICO_CAMERA_DEFAULT_CONFIG();
    cfg.slot = MOSAICO_MODULE_MGR_SLOT_LEFT;
    cfg.frame_timeout_ms = FRAME_TIMEOUT_MS;   /* the SC101IOT can be slow to deliver its first frame */
    esp_err_t err = rgb ? mosaico_camera_new(&cfg, &cam) : ESP_ERR_NO_MEM;
    if (rgb) {
        c.present = err == ESP_OK;   /* plugged or unplugged since boot */
        if (err != ESP_OK) {
            err = ESP_ERR_NOT_FOUND;
        }
    }
    if (err == ESP_OK) {
        err = mosaico_camera_open(cam);
    }
    if (err == ESP_OK) {
        err = mosaico_camera_start_stream(cam);
    }
    int w = 0, h = 0;
    /* A stream that never delivers a frame gets one restart (what the BSP's own
     * camera app does), then we give up. */
    for (int attempt = 0; err == ESP_OK && attempt < 2; ++attempt) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "no frames from the camera, restarting the stream");
            err = mosaico_camera_restart(cam);
            if (err != ESP_OK) {
                break;
            }
        }
        err = mosaico_camera_discard_frames(cam, SETTLE_FRAMES);
        if (err == ESP_OK) {
            mosaico_camera_frame_t fr = { 0 };
            err = mosaico_camera_get_frame(cam, &fr);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "frame %" PRIu32 "x%" PRIu32, fr.width, fr.height);
                err = convert(&fr, rgb, out_size, &w, &h);
                const esp_err_t r = mosaico_camera_return_frame(cam, &fr);
                if (err == ESP_OK) {
                    err = r;
                }
            }
        }
        if (err != ESP_ERR_TIMEOUT) {
            break;
        }
        if (attempt == 0) {
            err = ESP_OK;   /* retry after the restart */
        }
    }
    if (cam) {
        mosaico_camera_stop_stream(cam);
        mosaico_camera_close(cam);
        mosaico_camera_del(cam);
    }
    mosaico_module_mgr_deinit();
    ppa_unregister_client(c.ppa);
    c.ppa = NULL;
    if (err == ESP_OK) {
        err = encode(rgb, w, h, jpeg, len);
    }
    heap_caps_free(rgb);
    xSemaphoreGive(c.lock);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "photo %dx%d, %u bytes", w, h, (unsigned)*len);
    } else {
        ESP_LOGW(TAG, "capture failed: %s", esp_err_to_name(err));
    }
    return err;
}
