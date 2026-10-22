/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_ai_infer.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_VEDC_AI_CORESET_MAX_BANK
#define APP_AI_CORESET_MAX_BANK 64U
#else
#define APP_AI_CORESET_MAX_BANK CONFIG_VEDC_AI_CORESET_MAX_BANK
#endif
#define APP_AI_CORESET_MIN_BANK 32U
#define APP_AI_CORESET_RATIO 0.25f
#define APP_AI_CORESET_EPSILON_ALPHA 0.20f
#define APP_AI_CORESET_EPSILON_MIN 0.01f
#define APP_AI_CORESET_THRESHOLD_MARGIN 1.20f
#define APP_AI_CORESET_PREVIEW_DIMS 8U

typedef struct {
    const float *vectors;     /* count * APP_AI_INFER_FEATURE_DIM floats */
    uint32_t count;
    uint32_t source_image;    /* zero-based image index */
} app_ai_coreset_block_t;

typedef struct {
    float vector[APP_AI_INFER_FEATURE_DIM];
    uint32_t source_image;
    uint32_t source_patch;
    float selection_distance;
} app_ai_coreset_entry_t;

typedef struct {
    uint32_t bank_count;
    uint32_t total_vectors;
    uint32_t target_bank;
    uint32_t min_bank;
    uint32_t source_images;
    int64_t build_us;
    float threshold;
    float train_max_score;
    float cross_image_max_score;
    float epsilon;
    app_ai_coreset_entry_t entries[APP_AI_CORESET_MAX_BANK];
} app_ai_coreset_snapshot_t;

esp_err_t app_ai_coreset_init(void);
void app_ai_coreset_reset(void);

esp_err_t app_ai_coreset_build(const app_ai_coreset_block_t *blocks,
                               uint32_t block_count);

bool app_ai_coreset_is_ready(void);
uint32_t app_ai_coreset_get_bank_count(void);
float app_ai_coreset_get_threshold(void);
esp_err_t app_ai_coreset_export_snapshot(app_ai_coreset_snapshot_t *out);
esp_err_t app_ai_coreset_import_snapshot(const app_ai_coreset_snapshot_t *snapshot);

/* Tăng tốc tính khoảng cách nhỏ nhất (Anomaly Score) của 1 vector 32-D tới Memory Bank dùng ESP-DSP SIMD */
float app_ai_coreset_compute_score(const float *vector);
float app_ai_coreset_compute_diagnostics(const float *vector,
                                         float *d1, float *d3);

esp_err_t app_ai_coreset_get_status_json(char *buf, size_t len);
esp_err_t app_ai_coreset_get_bank_json(char *buf, size_t len);

esp_err_t app_ai_coreset_get_vector(uint32_t index,
                                    float *out,
                                    size_t out_len,
                                    uint32_t *source_image,
                                    uint32_t *source_patch,
                                    float *selection_distance);

#ifdef __cplusplus
}
#endif
