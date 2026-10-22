#pragma once

#include <stdint.h>
#include "esp_err.h"

#define APP_TRAIN_SAMPLE_COUNT 5U
#define APP_TRAIN_DONE_SIZE 26U

typedef struct {
    float imax_mean;
    float imax_std;
    float imax_max_dev;
    float iavg_mean;
    float iavg_std;
    float iavg_max_dev;
    float sample_imax[APP_TRAIN_SAMPLE_COUNT];
    float sample_iavg[APP_TRAIN_SAMPLE_COUNT];
    uint32_t feature_count[APP_TRAIN_SAMPLE_COUNT];
    uint32_t elapsed_ms;
    uint8_t train_done[APP_TRAIN_DONE_SIZE];
} app_train_params_t;

esp_err_t app_train_store_save(const app_train_params_t *params);
esp_err_t app_train_store_load(app_train_params_t *params);
esp_err_t app_train_store_clear(void);
