/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fixed normalized PCB image and overlapping patch geometry.
#define APP_AI_ROI_WIDTH         640
#define APP_AI_ROI_HEIGHT        448
#define APP_AI_PATCH_SIZE         64
#define APP_AI_PATCH_STRIDE       48
#define APP_AI_PATCH_CHANNELS      3
#define APP_AI_PATCH_MAX_COLS     (1 + (APP_AI_ROI_WIDTH - APP_AI_PATCH_SIZE) / APP_AI_PATCH_STRIDE)
#define APP_AI_PATCH_MAX_ROWS     (1 + (APP_AI_ROI_HEIGHT - APP_AI_PATCH_SIZE) / APP_AI_PATCH_STRIDE)
#define APP_AI_PATCH_MAX_COUNT    (APP_AI_PATCH_MAX_COLS * APP_AI_PATCH_MAX_ROWS)
#define APP_AI_PATCH_MAX_CROP_W   APP_AI_ROI_WIDTH
#define APP_AI_PATCH_MAX_CROP_H   APP_AI_ROI_HEIGHT

// Shared row-major patch geometry. Coordinates are relative to the crop.
void app_ai_patch_get_xy(uint32_t col, uint32_t row, uint32_t *x, uint32_t *y);
void app_ai_patch_get_roi_origin(uint32_t *x, uint32_t *y);
void app_ai_patch_get_source_size(uint32_t *w, uint32_t *h);

// Allocates the (PSRAM) patch pool. Call once from app_main() before the
// button/web handlers can trigger a capture.
esp_err_t app_ai_patch_init(void);

// Legacy synchronization API retained for compatibility. Supplied crop values
// cannot change the fixed 640x448 normalized PCB geometry.
esp_err_t app_ai_patch_set_crop_percent(float left, float top, float width, float height);

// Grabs one 640x480 frame, detects and straightens the PCB into 640x448 RGB,
// then cuts overlapping 64x64 patches at a 48-pixel stride (13x9).
// Does NOT run model inference.
// Blocking; safe to call from a normal task context (not an ISR).
esp_err_t app_ai_patch_capture(void);

// Same as app_ai_patch_capture(), but runs in its own short-lived task so
// the caller (e.g. the physical-button debounce loop) is not blocked while
// the JPEG decode/patch-cut runs.
esp_err_t app_ai_patch_capture_async(void);

// Re-splits an already normalized 640x448 RGB888 image into the
// same overlapping grid used by app_ai_patch_capture(). This is available
// for replaying a stored RGB crop and comparing the logged pool checksum.
esp_err_t app_ai_patch_process_crop_rgb(const uint8_t *crop_rgb, uint32_t crop_w, uint32_t crop_h);

// Gets the fixed normalized image percentages (0,0,100,100).
esp_err_t app_ai_patch_get_crop_percent(float *left, float *top, float *width, float *height);

// Number of valid patches produced by the last successful capture or crop-processing pass.
uint32_t app_ai_patch_get_count(void);

// Grid layout of the last successful capture or crop-processing pass.
void app_ai_patch_get_grid(uint32_t *cols, uint32_t *rows);

// Raw pointer to patch `index` (row-major, index = row * cols + col).
// Each patch is APP_AI_PATCH_SIZE * APP_AI_PATCH_SIZE * APP_AI_PATCH_CHANNELS
// bytes of packed RGB888, ready to hand to a model later. Returns NULL if
// index is out of range or no capture has succeeded yet.
const uint8_t *app_ai_patch_get_data(uint32_t index, size_t *len);

// Pointer to the exact normalized RGB888 image before its patch grid split.
// The Web UI image and AI patches therefore use identical pixels. *w/*h are
// filled in with its pixel size. Returns NULL (and leaves *w/*h/
// *len untouched) if no successful capture has produced one yet.
const uint8_t *app_ai_patch_get_crop_image(uint32_t *w, uint32_t *h, size_t *len);

// Assign the capture ID to the normalized image that produced the AI patches.
void app_ai_patch_mark_capture(uint32_t capture_id);
// Hide the normalized preview for a deleted LEARN sample if it is still current.
void app_ai_patch_forget_capture(uint32_t capture_id);
// Copy the exact normalized RGB used by AI as a 24-bit BMP for the Web UI.
esp_err_t app_ai_patch_copy_normalized_bmp(uint32_t capture_id, uint8_t **bmp, size_t *len);

// Builds a small JSON status object describing the last capture (or lack
// thereof) into buf. Used by the web UI to show capture results.
esp_err_t app_ai_patch_get_status_json(char *buf, size_t len);
void app_ai_patch_debug_dump(uint32_t index);

#ifdef __cplusplus
}
#endif
