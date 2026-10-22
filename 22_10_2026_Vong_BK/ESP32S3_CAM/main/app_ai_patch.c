/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * On-device (ESP32-S3) pipeline that turns whatever the camera currently
 * sees into a grid of APP_AI_PATCH_SIZE x APP_AI_PATCH_SIZE RGB888
 * patches, ready to be fed one-by-one into the teammate's AI model later.
 *
 * This module owns the camera->JPEG->PCB detection/normalization->64x64 patch pipeline. Model inference
 * remains in app_ai_infer.cpp; the controller runs it after each capture.
 *
 * Pipeline for one capture:
 *   1. Grab one MJPEG frame straight from the UVC driver (app_uvc_get_frame),
 *      independent of the live browser preview.
 *   2. Decode the JPEG on-device with the espressif/esp_jpeg component
 *      (TJpgDec) into a full-frame RGB888 buffer in PSRAM.
 *   3. Detect PCB corners and normalize into 640x448 RGB.
 *   4. Cut 13x9 overlapping 64x64 windows at a 48-pixel stride.
 *
 * Steps 3 and 4 both read from the one frame decoded in step 2, so the
 * "captured photo" and the 64x64 patches always come from the exact same
 * on-device capture - unlike an earlier version of this pipeline, where the
 * browser took its own canvas snapshot of the *live* preview stream while
 * this code independently grabbed a different UVC frame, so the "photo"
 * and the patches could silently drift apart if anything in the scene
 * moved between the two.
 */

#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "app_ai_patch.h"
#include "app_uvc_manager.h"
#include "pcb_preprocess.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"

// Sanity cap on the decoded (full-frame) RGB888 buffer so a mis-negotiated
// huge resolution can't blow the PSRAM budget: ~4 MB covers well beyond any
// resolution the Logitech C270 actually offers over this USB path.
#define APP_AI_PATCH_MAX_DECODE_BYTES (4 * 1024 * 1024)

// How long to wait for a fresh UVC frame before giving up.
#define APP_AI_PATCH_FRAME_WAIT_MS (1500)

// app_uvc_get_frame() dequeues from ONE shared FreeRTOS queue that the
// another camera consumer may be continuously draining. When both tasks are
// blocked waiting on that queue, FreeRTOS always wakes the higher-priority
// one first - so as long as a browser tab has the camera page open, it wins
// every single frame and this capture starves forever.
// Fix: briefly raise this task's own priority above the streaming task's
// (5) only while waiting for the frame, then restore it right away. This
// must stay higher than the camera consumer's priority.
#define APP_AI_PATCH_FRAME_GRAB_PRIORITY (6)

static const char *TAG = "app_ai_patch";

typedef struct {
    float left;
    float top;
    float width;
    float height;
} app_ai_patch_crop_t;

static struct {
    SemaphoreHandle_t lock;
    uint8_t *pool;                 // APP_AI_PATCH_MAX_COUNT patches, back to back
    app_ai_patch_crop_t crop;      // percent, same convention as the web UI
    uint32_t cols;
    uint32_t rows;
    uint32_t count;                // cols * rows of the last successful capture
    uint32_t source_w;
    uint32_t source_h;
    uint32_t roi_x;
    uint32_t roi_y;
    int64_t last_capture_us;
    bool last_capture_ok;
    char last_error[48];
    // Exact normalized RGB image used for both patches and web inspection.
    uint8_t *crop_image;
    size_t crop_image_cap;
    uint32_t crop_image_w;
    uint32_t crop_image_h;
    uint32_t normalized_capture_id;
} s_ai = {
    .crop = { .left = 0.0f, .top = 0.0f, .width = 100.0f, .height = 100.0f },
};

// ---------------------------------------------------------------------
// Fix: many UVC webcams (this Logitech C270 included) send MJPEG frames
// that only contain a DQT (quantization table) marker and NO DHT (Huffman
// table) marker. That is legal MJPEG - the Motion-JPEG convention is that
// the receiver already knows the "standard" JPEG Huffman tables from
// ITU-T.81 Annex K.3 and is expected to supply them itself. Browsers'
// built-in JPEG decoders do this silently, which is why the live preview
// in this project (raw bytes forwarded straight to the browser over the
// WebSocket) always looks fine. The on-device decoder (espressif/esp_jpeg)
// does NOT do this substitution, so every single on-device decode call
// fails with "Error in preparing JPEG image! 6" as soon as it hits a
// table-less frame - which, for this camera, is every frame.
//
// The fix is to inspect each captured frame and, if it has no DHT marker,
// splice the standard tables in (right after the SOI marker, before
// anything else) so esp_jpeg_decode() gets a fully self-contained JPEG.
// ---------------------------------------------------------------------

static const uint8_t s_dht_bits_dc_luma[16]     = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t s_dht_vals_dc_luma[12]     = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t s_dht_bits_dc_chroma[16]   = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const uint8_t s_dht_vals_dc_chroma[12]   = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t s_dht_bits_ac_luma[16]     = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const uint8_t s_dht_vals_ac_luma[162]    = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
};
static const uint8_t s_dht_bits_ac_chroma[16]   = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const uint8_t s_dht_vals_ac_chroma[162]  = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
};

// FFC4 + 2 length bytes + 4 tables, each (1 id-byte + 16 count-bytes + N value-bytes)
#define APP_AI_PATCH_STD_DHT_SIZE \
    (2 + 2 + (1 + 16 + sizeof(s_dht_vals_dc_luma)) + (1 + 16 + sizeof(s_dht_vals_dc_chroma)) + \
     (1 + 16 + sizeof(s_dht_vals_ac_luma)) + (1 + 16 + sizeof(s_dht_vals_ac_chroma)))

static size_t append_dht_table(uint8_t *out, uint8_t class_and_id, const uint8_t *bits, const uint8_t *vals, size_t n_vals)
{
    size_t pos = 0;
    out[pos++] = class_and_id;
    memcpy(out + pos, bits, 16);
    pos += 16;
    memcpy(out + pos, vals, n_vals);
    pos += n_vals;
    return pos;
}

// Builds the standard-Huffman-tables DHT marker segment once and caches it;
// the bytes are constant so there is no need to rebuild them per frame.
static const uint8_t *get_standard_dht_segment(size_t *out_len)
{
    static uint8_t s_segment[APP_AI_PATCH_STD_DHT_SIZE];
    static bool s_ready = false;
    if (!s_ready) {
        size_t pos = 0;
        s_segment[pos++] = 0xFF;
        s_segment[pos++] = 0xC4;
        size_t len_pos = pos;
        pos += 2; // filled in below, once payload length is known
        pos += append_dht_table(s_segment + pos, 0x00, s_dht_bits_dc_luma, s_dht_vals_dc_luma, sizeof(s_dht_vals_dc_luma));
        pos += append_dht_table(s_segment + pos, 0x01, s_dht_bits_dc_chroma, s_dht_vals_dc_chroma, sizeof(s_dht_vals_dc_chroma));
        pos += append_dht_table(s_segment + pos, 0x10, s_dht_bits_ac_luma, s_dht_vals_ac_luma, sizeof(s_dht_vals_ac_luma));
        pos += append_dht_table(s_segment + pos, 0x11, s_dht_bits_ac_chroma, s_dht_vals_ac_chroma, sizeof(s_dht_vals_ac_chroma));
        uint16_t seg_len = (uint16_t)(pos - len_pos); // length field covers itself + everything after it
        s_segment[len_pos] = (uint8_t)(seg_len >> 8);
        s_segment[len_pos + 1] = (uint8_t)(seg_len & 0xFF);
        s_ready = true;
    }
    *out_len = sizeof(s_segment);
    return s_segment;
}

// Scans the marker segments between SOI and SOS looking for an existing
// DHT (0xFFC4) marker. Stops as soon as it reaches the entropy-coded scan
// data (SOS) or runs out of well-formed markers.
static bool jpeg_has_dht(const uint8_t *data, size_t len)
{
    if (len < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        return false;
    }
    size_t pos = 2;
    while (pos + 1 < len) {
        if (data[pos] != 0xFF) {
            pos++;
            continue;
        }
        uint8_t marker = data[pos + 1];
        if (marker == 0xFF) { // fill byte
            pos++;
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
            // TEM / RSTn / SOI: no length field
            pos += 2;
            continue;
        }
        if (marker == 0xD9) { // EOI
            return false;
        }
        if (marker == 0xDA) { // SOS - entropy data starts, DHT must appear before this
            return false;
        }
        if (marker == 0xC4) {
            return true;
        }
        if (pos + 4 > len) {
            return false;
        }
        uint16_t seg_len = ((uint16_t)data[pos + 2] << 8) | data[pos + 3];
        if (seg_len < 2) {
            return false; // malformed segment length, give up
        }
        pos += 2 + seg_len;
    }
    return false;
}

// If `data` is missing its DHT marker, returns a freshly-allocated copy
// (SOI + standard DHT segment + the rest of the original bytes) that the
// caller must free. Returns NULL (and leaves *out_len untouched) if the
// frame already has its own tables or the allocation failed - in the
// latter case the caller should just try decoding the original data as-is.
static uint8_t *jpeg_ensure_dht(const uint8_t *data, size_t len, size_t *out_len)
{
    static bool s_logged = false;
    if (jpeg_has_dht(data, len)) {
        return NULL;
    }
    if (!s_logged) {
        ESP_LOGI(TAG, "Camera MJPEG frames have no embedded Huffman table (common for UVC webcams); "
                 "injecting standard JPEG tables before on-device decode");
        s_logged = true;
    }
    if (len < 2) {
        return NULL;
    }
    size_t dht_len = 0;
    const uint8_t *dht = get_standard_dht_segment(&dht_len);
    size_t total = 2 + dht_len + (len - 2);
    uint8_t *buf = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = heap_caps_malloc(total, MALLOC_CAP_8BIT);
    }
    if (!buf) {
        ESP_LOGW(TAG, "Allocate %u byte DHT-fix buffer failed, decoding frame as-is", (unsigned)total);
        return NULL;
    }
    memcpy(buf, data, 2);                 // SOI
    memcpy(buf + 2, dht, dht_len);         // injected standard Huffman tables
    memcpy(buf + 2 + dht_len, data + 2, len - 2); // rest of the original frame
    *out_len = total;
    return buf;
}

void app_ai_patch_get_xy(uint32_t col, uint32_t row, uint32_t *x, uint32_t *y)
{
    if (x) *x = col * APP_AI_PATCH_STRIDE;
    if (y) *y = row * APP_AI_PATCH_STRIDE;
}

void app_ai_patch_get_roi_origin(uint32_t *x, uint32_t *y)
{
    if (!s_ai.lock) return;
    xSemaphoreTake(s_ai.lock, portMAX_DELAY);
    if (x) *x = s_ai.roi_x;
    if (y) *y = s_ai.roi_y;
    xSemaphoreGive(s_ai.lock);
}

void app_ai_patch_get_source_size(uint32_t *w, uint32_t *h)
{
    if (!s_ai.lock) return;
    xSemaphoreTake(s_ai.lock, portMAX_DELAY);
    if (w) *w = s_ai.source_w;
    if (h) *h = s_ai.source_h;
    xSemaphoreGive(s_ai.lock);
}

static size_t patch_stride_bytes(void)
{
    return (size_t)APP_AI_PATCH_SIZE * APP_AI_PATCH_SIZE * APP_AI_PATCH_CHANNELS;
}

static uint32_t patch_pool_hash(uint32_t count)
{
    uint32_t hash = 2166136261u;
    const size_t bytes = (size_t)count * patch_stride_bytes();
    for (size_t i = 0; i < bytes; ++i) {
        hash = (hash ^ s_ai.pool[i]) * 16777619u;
    }
    return hash;
}

esp_err_t app_ai_patch_init(void)
{
    if (s_ai.lock) {
        return ESP_OK; // already initialized
    }
    s_ai.lock = xSemaphoreCreateMutex();
    if (!s_ai.lock) {
        ESP_LOGE(TAG, "Create AI patch mutex failed");
        return ESP_ERR_NO_MEM;
    }

    size_t pool_bytes = (size_t)APP_AI_PATCH_MAX_COUNT * patch_stride_bytes();
    s_ai.pool = heap_caps_malloc(pool_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ai.pool) {
        ESP_LOGW(TAG, "PSRAM allocation for patch pool failed (%u bytes), trying internal RAM", (unsigned)pool_bytes);
        s_ai.pool = heap_caps_malloc(pool_bytes, MALLOC_CAP_8BIT);
    }
    if (!s_ai.pool) {
        ESP_LOGE(TAG, "Allocate AI patch pool failed (%u bytes)", (unsigned)pool_bytes);
        vSemaphoreDelete(s_ai.lock);
        s_ai.lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "AI patch pool ready: ROI=%ux%u grid=%ux%u patch=%u stride=%u pool=%u bytes free_psram=%u largest_psram=%u",
             APP_AI_ROI_WIDTH, APP_AI_ROI_HEIGHT, APP_AI_PATCH_MAX_COLS, APP_AI_PATCH_MAX_ROWS,
             APP_AI_PATCH_SIZE, APP_AI_PATCH_STRIDE, (unsigned)pool_bytes,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

esp_err_t app_ai_patch_set_crop_percent(float left, float top, float width, float height)
{
    (void)left;
    (void)top;
    (void)width;
    (void)height;
    // Compatibility API: the AI crop is derived from frame dimensions.
    if (!s_ai.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

// Copy one source window into a packed patch. Current geometry always takes
// the direct-copy path because source and model patch dimensions are equal.
static void copy_patch_rgb(const uint8_t *src, int src_stride,
                           int src_x0, int src_y0, uint8_t *dst)
{
    const size_t row_bytes = (size_t)APP_AI_PATCH_SIZE * APP_AI_PATCH_CHANNELS;
    for (int y = 0; y < APP_AI_PATCH_SIZE; y++) {
        const uint8_t *src_row = src +
            (size_t)(src_y0 + y) * (size_t)src_stride * APP_AI_PATCH_CHANNELS +
            (size_t)src_x0 * APP_AI_PATCH_CHANNELS;
        memcpy(dst + (size_t)y * row_bytes, src_row, row_bytes);
    }
}


esp_err_t app_ai_patch_process_crop_rgb(const uint8_t *crop_rgb, uint32_t crop_w, uint32_t crop_h)
{
    if (!s_ai.lock || !s_ai.pool || !crop_rgb) {
        ESP_LOGW(TAG, "AI crop processing skipped: module not initialized or invalid buffer");
        return ESP_ERR_INVALID_STATE;
    }
    if (crop_w == 0 || crop_h == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (crop_w != APP_AI_ROI_WIDTH || crop_h != APP_AI_ROI_HEIGHT) {
        ESP_LOGE(TAG, "AI stored normalized image must be %dx%d; got %ux%u",
                 APP_AI_PATCH_MAX_CROP_W, APP_AI_PATCH_MAX_CROP_H, (unsigned)crop_w, (unsigned)crop_h);
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t cols = 1 + (crop_w - APP_AI_PATCH_SIZE) / APP_AI_PATCH_STRIDE;
    const uint32_t rows = 1 + (crop_h - APP_AI_PATCH_SIZE) / APP_AI_PATCH_STRIDE;

    xSemaphoreTake(s_ai.lock, portMAX_DELAY);
    size_t image_bytes = (size_t)crop_w * crop_h * APP_AI_PATCH_CHANNELS;
    if (image_bytes > s_ai.crop_image_cap) {
        uint8_t *bigger = heap_caps_realloc(s_ai.crop_image, image_bytes,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!bigger) {
            xSemaphoreGive(s_ai.lock);
            return ESP_ERR_NO_MEM;
        }
        s_ai.crop_image = bigger;
        s_ai.crop_image_cap = image_bytes;
    }
    memcpy(s_ai.crop_image, crop_rgb, image_bytes);
    s_ai.crop_image_w = crop_w;
    s_ai.crop_image_h = crop_h;
    s_ai.normalized_capture_id = 0;
    for (uint32_t r = 0; r < rows; r++) {
        uint32_t cell_y0, unused_y;
        app_ai_patch_get_xy(0, r, &unused_y, &cell_y0);
        for (uint32_t c = 0; c < cols; c++) {
            uint32_t cell_x0, unused_x;
            app_ai_patch_get_xy(c, r, &cell_x0, &unused_x);
            uint32_t idx = r * cols + c;
            uint8_t *dst = s_ai.pool + (size_t)idx * patch_stride_bytes();
            // Source window is exactly 64x64, so this is a direct
            // copy through the common helper (no geometric distortion).
            copy_patch_rgb(crop_rgb, (int)crop_w, cell_x0, cell_y0, dst);
        }
    }
    const uint32_t hash = patch_pool_hash(cols * rows);
    s_ai.cols = cols;
    s_ai.rows = rows;
    s_ai.count = cols * rows;
    s_ai.source_w = crop_w;
    s_ai.source_h = crop_h;
    s_ai.roi_x = 0;
    s_ai.roi_y = 0;
    s_ai.crop = (app_ai_patch_crop_t){.left = 0.0f, .top = 0.0f, .width = 100.0f, .height = 100.0f};
    s_ai.last_capture_ok = true;
    s_ai.last_capture_us = esp_timer_get_time();
    s_ai.last_error[0] = '\0';
    xSemaphoreGive(s_ai.lock);

    ESP_LOGI(TAG, "AI patch process crop ok: crop=%ux%u grid=%ux%u (%u patches of %dx%d) pool_fnv1a=%08lx",
             (unsigned)crop_w, (unsigned)crop_h, (unsigned)cols, (unsigned)rows,
             (unsigned)(cols * rows), APP_AI_PATCH_SIZE, APP_AI_PATCH_SIZE, (unsigned long)hash);
    return ESP_OK;
}

esp_err_t app_ai_patch_get_crop_percent(float *left, float *top, float *width, float *height)
{
    if (!left || !top || !width || !height || !s_ai.lock) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_ai.lock, portMAX_DELAY);
    *left = s_ai.crop.left;
    *top = s_ai.crop.top;
    *width = s_ai.crop.width;
    *height = s_ai.crop.height;
    xSemaphoreGive(s_ai.lock);
    return ESP_OK;
}

esp_err_t app_ai_patch_capture(void)
{
    if (!s_ai.lock || !s_ai.pool) {
        ESP_LOGW(TAG, "AI patch capture skipped: not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t capture_start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "[CAPTURE] waiting UVC MJPEG frame (timeout=%d ms)", APP_AI_PATCH_FRAME_WAIT_MS);
    xSemaphoreTake(s_ai.lock, portMAX_DELAY);
    s_ai.normalized_capture_id = 0;
    s_ai.last_capture_ok = false;
    xSemaphoreGive(s_ai.lock);
    if (!app_uvc_is_streaming() || app_uvc_get_format() != APP_UVC_FORMAT_MJPEG) {
        ESP_LOGW(TAG, "AI patch capture skipped: camera not streaming MJPEG");
        xSemaphoreTake(s_ai.lock, portMAX_DELAY);
        s_ai.last_capture_ok = false;
        snprintf(s_ai.last_error, sizeof(s_ai.last_error), "camera_not_streaming");
        xSemaphoreGive(s_ai.lock);
        return ESP_ERR_INVALID_STATE;
    }

    UBaseType_t orig_prio = uxTaskPriorityGet(NULL);
    bool boosted = orig_prio < APP_AI_PATCH_FRAME_GRAB_PRIORITY;
    if (boosted) {
        vTaskPrioritySet(NULL, APP_AI_PATCH_FRAME_GRAB_PRIORITY);
    }
    uvc_host_frame_t *frame = app_uvc_get_frame(pdMS_TO_TICKS(APP_AI_PATCH_FRAME_WAIT_MS));
    if (boosted) {
        vTaskPrioritySet(NULL, orig_prio);
    }
    if (!frame) {
        ESP_LOGW(TAG, "AI patch capture failed: no camera frame available");
        xSemaphoreTake(s_ai.lock, portMAX_DELAY);
        s_ai.last_capture_ok = false;
        snprintf(s_ai.last_error, sizeof(s_ai.last_error), "frame_timeout");
        xSemaphoreGive(s_ai.lock);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "[CAPTURE] frame received %ux%u JPEG=%u bytes after %.1f ms",
             frame->vs_format.h_res, frame->vs_format.v_res, (unsigned)frame->data_len,
             (double)(esp_timer_get_time() - capture_start_us) / 1000.0);

    esp_err_t ret = ESP_OK;
    const char *err_code = NULL;
    uint8_t *decoded = NULL;
    uint8_t *dht_fixed = NULL; // only set if the raw frame needed patched Huffman tables

    size_t decode_bytes = (size_t)frame->vs_format.h_res * frame->vs_format.v_res * APP_AI_PATCH_CHANNELS;
    if (decode_bytes == 0 || decode_bytes > APP_AI_PATCH_MAX_DECODE_BYTES) {
        ESP_LOGE(TAG, "AI patch capture failed: unsupported frame size %ux%u",
                 frame->vs_format.h_res, frame->vs_format.v_res);
        ret = ESP_ERR_INVALID_SIZE;
        err_code = "resolution_out_of_range";
        goto done;
    }

    decoded = heap_caps_malloc(decode_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!decoded) {
        decoded = heap_caps_malloc(decode_bytes, MALLOC_CAP_8BIT);
    }
    if (!decoded) {
        ESP_LOGE(TAG, "AI patch capture failed: allocate %u byte decode buffer failed", (unsigned)decode_bytes);
        ret = ESP_ERR_NO_MEM;
        err_code = "no_mem_decode";
        goto done;
    }

    // This camera's MJPEG frames commonly omit the Huffman tables (DHT) -
    // browsers substitute the JPEG standard tables silently, but the
    // on-device decoder needs them spliced in explicitly or it fails with
    // "Error in preparing JPEG image".
    size_t decode_indata_len = frame->data_len;
    uint8_t *decode_indata = frame->data;
    dht_fixed = jpeg_ensure_dht(frame->data, frame->data_len, &decode_indata_len);
    if (dht_fixed) {
        decode_indata = dht_fixed;
    } else {
        decode_indata_len = frame->data_len;
    }

    esp_jpeg_image_cfg_t jpeg_cfg = {
        .indata = decode_indata,
        .indata_size = decode_indata_len,
        .outbuf = decoded,
        .outbuf_size = decode_bytes,
        .out_format = JPEG_IMAGE_FORMAT_RGB888,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = {
            .swap_color_bytes = 0,
        },
    };
    esp_jpeg_image_output_t out_info = {0};
    ret = esp_jpeg_decode(&jpeg_cfg, &out_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AI patch capture failed: JPEG decode error %s", esp_err_to_name(ret));
        err_code = "jpeg_decode_failed";
        goto done;
    }
    ESP_LOGI(TAG, "[CAPTURE] JPEG decoded RGB=%ux%u after %.1f ms",
             out_info.width, out_info.height,
             (double)(esp_timer_get_time() - capture_start_us) / 1000.0);

    // Frame data is no longer needed once decoded - return it to the UVC
    // driver as early as possible so the camera pipeline keeps flowing.
    esp_err_t ret2 = app_uvc_return_frame(frame);
    frame = NULL;
    if (ret2 != ESP_OK) {
        ESP_LOGW(TAG, "Return UVC frame after AI decode failed: %s", esp_err_to_name(ret2));
    }

    int width = out_info.width;
    int height = out_info.height;
    const int crop_w = APP_AI_ROI_WIDTH, crop_h = APP_AI_ROI_HEIGHT;
    pcb_box_t board;
    if (width != 640 || height != 480) {
        ret = ESP_ERR_INVALID_SIZE;
        err_code = "pcb_requires_640x480";
        goto done;
    }
    if (!pcb_detect_rgb(decoded, width, height, &board)) {
        ESP_LOGW(TAG, "PCB not found; check board color, lighting and full edge visibility");
        ret = ESP_ERR_NOT_FOUND;
        err_code = "pcb_not_found";
        goto done;
    }

    ESP_LOGI(TAG, "PCB detected: foreground_pixels=%d angle=%.1f; PSRAM free=%u largest=%u decode=%u normalized=%ux%u",
             board.foreground_pixels, (double)board.angle_deg,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)decode_bytes, crop_w, crop_h);

    xSemaphoreTake(s_ai.lock, portMAX_DELAY);
    s_ai.crop = (app_ai_patch_crop_t){.left=0, .top=0, .width=100, .height=100};
    xSemaphoreGive(s_ai.lock);

    // Build the exact normalized RGB frame used by both patch cutting and
    // the read-only inspection image endpoint.
    {
        size_t row_bytes = (size_t)crop_w * APP_AI_PATCH_CHANNELS;
        size_t need = row_bytes * (size_t)crop_h;
        xSemaphoreTake(s_ai.lock, portMAX_DELAY);
        if (need > s_ai.crop_image_cap) {
            uint8_t *bigger = heap_caps_realloc(s_ai.crop_image, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!bigger) {
                bigger = heap_caps_realloc(s_ai.crop_image, need, MALLOC_CAP_8BIT);
            }
            if (bigger) {
                s_ai.crop_image = bigger;
                s_ai.crop_image_cap = need;
            }
        }
        if (s_ai.crop_image && need <= s_ai.crop_image_cap) {
            pcb_normalize_rgb(decoded, width, height, &board, s_ai.crop_image, crop_w, crop_h);
            s_ai.crop_image_w = (uint32_t)crop_w;
            s_ai.crop_image_h = (uint32_t)crop_h;
        } else {
            ESP_LOGE(TAG, "Allocate %u byte normalized RGB buffer failed", (unsigned)need);
            s_ai.crop_image_w = 0;
            s_ai.crop_image_h = 0;
            ret = ESP_ERR_NO_MEM;
            err_code = "no_mem_normalized";
        }
        xSemaphoreGive(s_ai.lock);
    }
    if (ret != ESP_OK) goto done;

    ESP_LOGI(TAG, "PSRAM before inference: free=%u largest=%u patch_pool=%u crop_rgb=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)((size_t)APP_AI_PATCH_MAX_COUNT * patch_stride_bytes()),
             (unsigned)((size_t)crop_w * crop_h * APP_AI_PATCH_CHANNELS));

    // 64x64 patches overlap by 16 pixels at the 48-pixel stride.
    uint32_t cols = APP_AI_PATCH_MAX_COLS;
    uint32_t rows = APP_AI_PATCH_MAX_ROWS;
    if (cols == 0 || rows == 0 || cols > APP_AI_PATCH_MAX_COLS || rows > APP_AI_PATCH_MAX_ROWS) {
        ESP_LOGE(TAG, "AI patch capture failed: crop grid out of bounds: crop=%dx%d grid=%ux%u",
                 crop_w, crop_h, (unsigned)cols, (unsigned)rows);
        ret = ESP_ERR_INVALID_SIZE;
        err_code = "fixed_grid_out_of_range";
        goto done;
    }

    xSemaphoreTake(s_ai.lock, portMAX_DELAY);
    for (uint32_t r = 0; r < rows; r++) {
        uint32_t local_y, unused_y;
        app_ai_patch_get_xy(0, r, &unused_y, &local_y);
        int cell_y0 = (int)local_y;
        for (uint32_t c = 0; c < cols; c++) {
            uint32_t local_x, unused_x;
            app_ai_patch_get_xy(c, r, &local_x, &unused_x);
            int cell_x0 = (int)local_x;
            uint32_t idx = r * cols + c;
            uint8_t *dst = s_ai.pool + (size_t)idx * patch_stride_bytes();
            copy_patch_rgb(s_ai.crop_image, crop_w, cell_x0, cell_y0, dst);
        }
    }
    const uint32_t hash = patch_pool_hash(cols * rows);
    s_ai.cols = cols;
    s_ai.rows = rows;
    s_ai.count = cols * rows;
    s_ai.source_w = (uint32_t)crop_w;
    s_ai.source_h = (uint32_t)crop_h;
    s_ai.roi_x = 0;
    s_ai.roi_y = 0;
    s_ai.last_capture_ok = true;
    s_ai.last_capture_us = esp_timer_get_time();
    s_ai.last_error[0] = '\0';
    xSemaphoreGive(s_ai.lock);

    ESP_LOGI(TAG, "AI patch capture ok: source=%dx%d normalized=%dx%d grid=%ux%u (%u overlapping patches of %dx%d) pool_fnv1a=%08lx capture_ms=%.1f",
             width, height, crop_w, crop_h, (unsigned)cols, (unsigned)rows,
             (unsigned)(cols * rows), APP_AI_PATCH_SIZE, APP_AI_PATCH_SIZE,
             (unsigned long)hash, (double)(esp_timer_get_time() - capture_start_us) / 1000.0);

done:
    if (frame) {
        esp_err_t ret3 = app_uvc_return_frame(frame);
        if (ret3 != ESP_OK) {
            ESP_LOGW(TAG, "Return UVC frame after AI capture error failed: %s", esp_err_to_name(ret3));
        }
    }
    if (decoded) {
        heap_caps_free(decoded);
    }
    if (dht_fixed) {
        heap_caps_free(dht_fixed);
    }
    if (ret != ESP_OK && err_code) {
        xSemaphoreTake(s_ai.lock, portMAX_DELAY);
        s_ai.last_capture_ok = false;
        snprintf(s_ai.last_error, sizeof(s_ai.last_error), "%s", err_code);
        xSemaphoreGive(s_ai.lock);
    }
    return ret;
}

static void ai_patch_capture_task(void *arg)
{
    (void)arg;
    esp_err_t ret = app_ai_patch_capture();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Async AI patch capture failed: %s", esp_err_to_name(ret));
    }
    vTaskDelete(NULL);
}

esp_err_t app_ai_patch_capture_async(void)
{
    if (!s_ai.lock || !s_ai.pool) {
        return ESP_ERR_INVALID_STATE;
    }
    // Stack needs to cover the JPEG decode (TJpgDec) working buffer plus the
    // patch-cutting loop; 6 KB matches the margin used elsewhere in this
    // project for similarly-sized image work.
    // Pinned to core 1: the WiFi/lwIP driver tasks always run on core 0 in
    // ESP-IDF, so keeping this CPU-heavy JPEG-decode + crop work on core 1
    // avoids contending with them for cycles/cache and gives more
    // consistent capture timing.
    if (xTaskCreatePinnedToCore(ai_patch_capture_task, "ai_patch_cap", 6144, NULL, 3, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Create async AI patch capture task failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

uint32_t app_ai_patch_get_count(void)
{
    if (!s_ai.lock) {
        return 0;
    }
    xSemaphoreTake(s_ai.lock, portMAX_DELAY);
    uint32_t count = s_ai.last_capture_ok ? s_ai.count : 0;
    xSemaphoreGive(s_ai.lock);
    return count;
}

void app_ai_patch_get_grid(uint32_t *cols, uint32_t *rows)
{
    uint32_t c = 0, r = 0;
    if (s_ai.lock) {
        xSemaphoreTake(s_ai.lock, portMAX_DELAY);
        if (s_ai.last_capture_ok) {
            c = s_ai.cols;
            r = s_ai.rows;
        }
        xSemaphoreGive(s_ai.lock);
    }
    if (cols) {
        *cols = c;
    }
    if (rows) {
        *rows = r;
    }
}

const uint8_t *app_ai_patch_get_data(uint32_t index, size_t *len)
{
    if (!s_ai.lock || !s_ai.pool) {
        return NULL;
    }
    const uint8_t *ptr = NULL;
    xSemaphoreTake(s_ai.lock, portMAX_DELAY);
    if (s_ai.last_capture_ok && index < s_ai.count) {
        ptr = s_ai.pool + (size_t)index * patch_stride_bytes();
    }
    xSemaphoreGive(s_ai.lock);
    if (ptr && len) {
        *len = patch_stride_bytes();
    }
    return ptr;
}

const uint8_t *app_ai_patch_get_crop_image(uint32_t *w, uint32_t *h, size_t *len)
{
    if (!s_ai.lock) {
        return NULL;
    }
    const uint8_t *ptr = NULL;
    uint32_t cw = 0, ch = 0;
    xSemaphoreTake(s_ai.lock, portMAX_DELAY);
    if (s_ai.last_capture_ok && s_ai.crop_image && s_ai.crop_image_w > 0 && s_ai.crop_image_h > 0) {
        ptr = s_ai.crop_image;
        cw = s_ai.crop_image_w;
        ch = s_ai.crop_image_h;
    }
    xSemaphoreGive(s_ai.lock);
    if (!ptr) {
        return NULL;
    }
    if (w) {
        *w = cw;
    }
    if (h) {
        *h = ch;
    }
    if (len) {
        *len = (size_t)cw * (size_t)ch * APP_AI_PATCH_CHANNELS;
    }
    return ptr;
}

void app_ai_patch_mark_capture(uint32_t capture_id)
{
    if (!s_ai.lock || capture_id == 0) return;
    xSemaphoreTake(s_ai.lock, portMAX_DELAY);
    if (s_ai.last_capture_ok && s_ai.crop_image) {
        s_ai.normalized_capture_id = capture_id;
    }
    xSemaphoreGive(s_ai.lock);
}

void app_ai_patch_forget_capture(uint32_t capture_id)
{
    if (!s_ai.lock || capture_id == 0) return;
    xSemaphoreTake(s_ai.lock, portMAX_DELAY);
    if (s_ai.normalized_capture_id == capture_id) {
        s_ai.normalized_capture_id = 0;
        s_ai.last_capture_ok = false;
    }
    xSemaphoreGive(s_ai.lock);
}

static void put_u32le(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

esp_err_t app_ai_patch_copy_normalized_bmp(uint32_t capture_id, uint8_t **bmp, size_t *len)
{
    if (!bmp || !len || !s_ai.lock) return ESP_ERR_INVALID_ARG;
    *bmp = NULL;
    *len = 0;
    xSemaphoreTake(s_ai.lock, portMAX_DELAY);
    if (!s_ai.last_capture_ok || !s_ai.crop_image || !s_ai.crop_image_w || !s_ai.crop_image_h ||
        (capture_id && capture_id != s_ai.normalized_capture_id)) {
        xSemaphoreGive(s_ai.lock);
        return ESP_ERR_NOT_FOUND;
    }
    uint32_t w = s_ai.crop_image_w, h = s_ai.crop_image_h;
    size_t row_bytes = ((size_t)w * 3 + 3) & ~(size_t)3;
    size_t total = 54 + row_bytes * h;
    uint8_t *data = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        xSemaphoreGive(s_ai.lock);
        return ESP_ERR_NO_MEM;
    }
    memset(data, 0, 54);
    data[0] = 'B'; data[1] = 'M';
    put_u32le(data + 2, (uint32_t)total);
    put_u32le(data + 10, 54);
    put_u32le(data + 14, 40);
    put_u32le(data + 18, w);
    put_u32le(data + 22, h);
    data[26] = 1; data[28] = 24;
    put_u32le(data + 34, (uint32_t)(row_bytes * h));
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t *src = s_ai.crop_image + (size_t)(h - 1 - y) * w * 3;
        uint8_t *dst = data + 54 + (size_t)y * row_bytes;
        for (uint32_t x = 0; x < w; ++x) {
            dst[x * 3] = src[x * 3 + 2];
            dst[x * 3 + 1] = src[x * 3 + 1];
            dst[x * 3 + 2] = src[x * 3];
        }
        if (row_bytes > (size_t)w * 3) memset(dst + (size_t)w * 3, 0, row_bytes - (size_t)w * 3);
    }
    xSemaphoreGive(s_ai.lock);
    *bmp = data;
    *len = total;
    return ESP_OK;
}

esp_err_t app_ai_patch_get_status_json(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ai.lock) {
        // Module never initialized successfully (e.g. PSRAM alloc failed at
        // boot) - report a well-formed "unavailable" status instead of
        // touching an uncreated mutex.
        int written = snprintf(buf, len,
                                "{\"ok\":false,\"error\":\"not_initialized\",\"patch_size\":%d,"
                                "\"max_cols\":%d,\"max_rows\":%d,"
                                "\"cols\":0,\"rows\":0,\"count\":0}",
                                APP_AI_PATCH_SIZE, APP_AI_PATCH_MAX_COLS, APP_AI_PATCH_MAX_ROWS);
        return (written > 0 && (size_t)written < len) ? ESP_OK : ESP_ERR_NO_MEM;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_ai.lock, portMAX_DELAY);
    bool ok = s_ai.last_capture_ok;
    uint32_t cols = s_ai.cols, rows = s_ai.rows, count = s_ai.count;
    uint32_t source_w = s_ai.source_w, source_h = s_ai.source_h;
    uint32_t roi_x = s_ai.roi_x, roi_y = s_ai.roi_y;
    int64_t last_us = s_ai.last_capture_us;
    app_ai_patch_crop_t crop = s_ai.crop;
    char err[sizeof(s_ai.last_error)];
    memcpy(err, s_ai.last_error, sizeof(err));
    xSemaphoreGive(s_ai.lock);

    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddNumberToObject(root, "roi_width", APP_AI_ROI_WIDTH);
    cJSON_AddNumberToObject(root, "roi_height", APP_AI_ROI_HEIGHT);
    cJSON_AddNumberToObject(root, "patch_size", APP_AI_PATCH_SIZE);
    cJSON_AddNumberToObject(root, "patch_stride", APP_AI_PATCH_STRIDE);
    // Exposed so the web UI can compute (and draw) the exact same grid the
    // device will cut *before* a capture happens, instead of guessing/
    // hard-coding these constants client-side.
    cJSON_AddNumberToObject(root, "max_cols", APP_AI_PATCH_MAX_COLS);
    cJSON_AddNumberToObject(root, "max_rows", APP_AI_PATCH_MAX_ROWS);
    cJSON_AddNumberToObject(root, "cols", cols);
    cJSON_AddNumberToObject(root, "rows", rows);
    cJSON_AddNumberToObject(root, "count", count);
    cJSON_AddNumberToObject(root, "source_width", source_w);
    cJSON_AddNumberToObject(root, "source_height", source_h);
    cJSON_AddNumberToObject(root, "roi_x", roi_x);
    cJSON_AddNumberToObject(root, "roi_y", roi_y);
    cJSON_AddNumberToObject(root, "last_capture_ms", (double)(last_us / 1000));
    if (!ok && err[0]) {
        cJSON_AddStringToObject(root, "error", err);
    }
    cJSON *crop_json = cJSON_AddObjectToObject(root, "crop");
    if (crop_json) {
        cJSON_AddNumberToObject(crop_json, "left", crop.left);
        cJSON_AddNumberToObject(crop_json, "top", crop.top);
        cJSON_AddNumberToObject(crop_json, "width", crop.width);
        cJSON_AddNumberToObject(crop_json, "height", crop.height);
        if (ok && source_w && source_h) {
            cJSON_AddNumberToObject(crop_json, "pixel_width", APP_AI_ROI_WIDTH);
            cJSON_AddNumberToObject(crop_json, "pixel_height", APP_AI_ROI_HEIGHT);
            cJSON_AddNumberToObject(crop_json, "pixel_x", roi_x);
            cJSON_AddNumberToObject(crop_json, "pixel_y", roi_y);
            cJSON_AddNumberToObject(crop_json, "patch_cols", cols);
            cJSON_AddNumberToObject(crop_json, "patch_rows", rows);
            cJSON_AddNumberToObject(crop_json, "patch_count", count);
        }
    }

    bool printed = cJSON_PrintPreallocated(root, buf, (int)len, false);
    cJSON_Delete(root);
    return printed ? ESP_OK : ESP_ERR_NO_MEM;
}

void app_ai_patch_debug_dump(uint32_t index)
{
    const uint8_t *ptr;
    size_t len;
    size_t i;

    ptr = app_ai_patch_get_data(index, &len);
    if (!ptr || len == 0) {
        ESP_LOGE("PATCH_DUMP", "[PATCH DUMP] patch=%lu unavailable (chua capture hoac sai index)", (unsigned long)index);
        return;
    }

    ESP_LOGI("PATCH_DUMP", "[PATCH DUMP] patch=%lu len=%lu",
           (unsigned long)index,
           (unsigned long)len);

    for (i = 0; i < len; i++) {
        printf("%02X", ptr[i]);

        if (i + 1 < len) {
            printf(",");
        }

        if ((i + 1) % 32 == 0) {
            printf("\n");
        }
    }

    printf("\n[PATCH DUMP END]\n");
    fflush(stdout); // FORCE FLUSH DỮ LIỆU RA SERIAL PORT NGAY LẬP TỨC
}
