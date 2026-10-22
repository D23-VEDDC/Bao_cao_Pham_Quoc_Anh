#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "app_train_store.h"
#include "app_ai_coreset.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TRAIN_PARTITION "train_store"
#define TRAIN_NAMESPACE "training"
#define TRAIN_KEY "model"
#define TRAIN_MAGIC 0x56454443U
#define TRAIN_VERSION 1U

static const char *TAG = "train_store";
static bool s_partition_ready;

extern const uint8_t model_start[] asm("_binary_pcb_embedding_64_i8in_f32out_tflite_start");
extern const uint8_t model_end[] asm("_binary_pcb_embedding_64_i8in_f32out_tflite_end");

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t length;
    uint32_t data_crc;
    uint32_t model_crc;
    app_train_params_t params;
    app_ai_coreset_snapshot_t coreset;
} train_record_t;

static esp_err_t init_partition(void)
{
    if (s_partition_ready) return ESP_OK;
    esp_err_t err = nvs_flash_init_partition(TRAIN_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Training partition needs reinitialization: %s", esp_err_to_name(err));
        err = nvs_flash_erase_partition(TRAIN_PARTITION);
        if (err == ESP_OK) err = nvs_flash_init_partition(TRAIN_PARTITION);
    }
    if (err == ESP_OK) s_partition_ready = true;
    return err;
}

static train_record_t *alloc_record(void)
{
    train_record_t *record = heap_caps_malloc(sizeof(train_record_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!record) record = heap_caps_malloc(sizeof(train_record_t), MALLOC_CAP_8BIT);
    return record;
}

static uint32_t model_crc(void)
{
    return esp_rom_crc32_le(0, model_start, (uint32_t)(model_end - model_start));
}

static bool valid_params(const app_train_params_t *params)
{
    if (!isfinite(params->imax_mean) || !isfinite(params->imax_std) ||
        !isfinite(params->imax_max_dev) || params->imax_max_dev < 0.0f ||
        !isfinite(params->iavg_mean) || !isfinite(params->iavg_std) ||
        !isfinite(params->iavg_max_dev) || params->iavg_max_dev < 0.0f) return false;
    for (uint32_t i = 0; i < APP_TRAIN_SAMPLE_COUNT; ++i) {
        if (!isfinite(params->sample_imax[i]) || !isfinite(params->sample_iavg[i]) ||
            params->feature_count[i] == 0) return false;
    }
    return true;
}

esp_err_t app_train_store_save(const app_train_params_t *params)
{
    if (!params || !valid_params(params)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = init_partition();
    if (err != ESP_OK) return err;
    train_record_t *record = alloc_record();
    if (!record) return ESP_ERR_NO_MEM;
    memset(record, 0, sizeof(*record));
    err = app_ai_coreset_export_snapshot(&record->coreset);
    if (err == ESP_OK) {
        record->magic = TRAIN_MAGIC;
        record->version = TRAIN_VERSION;
        record->model_crc = model_crc();
        record->params = *params;
        record->length = (uint32_t)(offsetof(train_record_t, coreset.entries) +
                          record->coreset.bank_count * sizeof(record->coreset.entries[0]));
        record->data_crc = esp_rom_crc32_le(0, (const uint8_t *)&record->model_crc,
                                           record->length - offsetof(train_record_t, model_crc));
        nvs_handle_t handle;
        err = nvs_open_from_partition(TRAIN_PARTITION, TRAIN_NAMESPACE, NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_blob(handle, TRAIN_KEY, record, record->length);
            if (err == ESP_OK) err = nvs_commit(handle);
            nvs_close(handle);
        }
    }
    if (err == ESP_OK) ESP_LOGI(TAG, "Saved %lu training vectors to flash (%lu bytes)",
                                (unsigned long)record->coreset.bank_count,
                                (unsigned long)record->length);
    heap_caps_free(record);
    return err;
}

esp_err_t app_train_store_load(app_train_params_t *params)
{
    if (!params) return ESP_ERR_INVALID_ARG;
    esp_err_t err = init_partition();
    if (err != ESP_OK) return err;
    nvs_handle_t handle;
    err = nvs_open_from_partition(TRAIN_PARTITION, TRAIN_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    size_t length = 0;
    err = nvs_get_blob(handle, TRAIN_KEY, NULL, &length);
    if (err != ESP_OK) { nvs_close(handle); return err; }
    if (length < offsetof(train_record_t, coreset.entries) + sizeof(app_ai_coreset_entry_t) ||
        length > sizeof(train_record_t)) {
        nvs_close(handle);
        return ESP_ERR_INVALID_SIZE;
    }
    train_record_t *record = alloc_record();
    if (!record) { nvs_close(handle); return ESP_ERR_NO_MEM; }
    memset(record, 0, sizeof(*record));
    err = nvs_get_blob(handle, TRAIN_KEY, record, &length);
    nvs_close(handle);
    if (err == ESP_OK &&
        (record->magic != TRAIN_MAGIC || record->version != TRAIN_VERSION ||
         record->length != length ||
         record->coreset.bank_count == 0 || record->coreset.bank_count > APP_AI_CORESET_MAX_BANK ||
         length != offsetof(train_record_t, coreset.entries) +
                   record->coreset.bank_count * sizeof(record->coreset.entries[0]) ||
         record->model_crc != model_crc() ||
         record->data_crc != esp_rom_crc32_le(0, (const uint8_t *)&record->model_crc,
                                              length - offsetof(train_record_t, model_crc)) ||
         !valid_params(&record->params))) {
        ESP_LOGW(TAG, "Stored training is incompatible or corrupted");
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) err = app_ai_coreset_import_snapshot(&record->coreset);
    if (err == ESP_OK) {
        *params = record->params;
        ESP_LOGI(TAG, "Restored %lu training vectors from flash",
                 (unsigned long)record->coreset.bank_count);
    }
    heap_caps_free(record);
    return err;
}

esp_err_t app_train_store_clear(void)
{
    esp_err_t err = init_partition();
    if (err != ESP_OK) return err;
    nvs_handle_t handle;
    err = nvs_open_from_partition(TRAIN_PARTITION, TRAIN_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(handle, TRAIN_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) ESP_LOGI(TAG, "Cleared previous training from flash");
    return err;
}
