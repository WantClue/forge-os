#include <string.h>

#include <esp_log.h>

#include "bm1370.h"
#include "common.h"
#include "global_state.h"

#include "asic.h"

static const char *TAG = "asic";

uint8_t ASIC_init(GlobalState * GLOBAL_STATE) {
    switch (GLOBAL_STATE->device_model) {
        case BITFORGE_NANO:
            return BM1370_init(GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value, BITFORGE_NANO_ASIC_COUNT);
        default:
    }
    return ESP_OK;
}

uint8_t ASIC_get_asic_count(GlobalState * GLOBAL_STATE) {
    switch (GLOBAL_STATE->device_model) {
        case BITFORGE_NANO:
            return BITFORGE_NANO_ASIC_COUNT;
        default:
    }
    return 0;
}

uint16_t ASIC_get_small_core_count(GlobalState * GLOBAL_STATE) {
    switch (GLOBAL_STATE->device_model) {
        case BITFORGE_NANO:
            return BM1370_SMALL_CORE_COUNT;
        default:
    }
    return 0;
}

// .receive_result_fn = BM1366_process_work,
task_result * ASIC_process_work(GlobalState * GLOBAL_STATE) {
    switch (GLOBAL_STATE->device_model) {
        case BITFORGE_NANO:
            return BM1370_process_work(GLOBAL_STATE);
        default:
    }
    return NULL;
}

// .set_max_baud_fn = BM1366_set_max_baud,
int ASIC_set_max_baud(GlobalState * GLOBAL_STATE) {
    switch (GLOBAL_STATE->device_model) {
        case BITFORGE_NANO:
            return BM1370_set_max_baud();
        default:
    return 0;
    }
}

// .set_difficulty_mask_fn = BM1366_set_job_difficulty_mask,
void ASIC_set_job_difficulty_mask(GlobalState * GLOBAL_STATE, uint8_t mask) {
    switch (GLOBAL_STATE->device_model) {
        case BITFORGE_NANO:
            BM1370_set_job_difficulty_mask(mask);
            break;
        default:
    }
}

// .send_work_fn = BM1366_send_work,
void ASIC_send_work(GlobalState * GLOBAL_STATE, void * next_job) {
    switch (GLOBAL_STATE->device_model) {
        case BITFORGE_NANO:
            BM1370_send_work(GLOBAL_STATE, next_job);
            break;
        default:
    return;
    }
}

// .set_version_mask = BM1366_set_version_mask
void ASIC_set_version_mask(GlobalState * GLOBAL_STATE, uint32_t mask) {
    switch (GLOBAL_STATE->device_model) {
        case BITFORGE_NANO:
            BM1370_set_version_mask(mask);
            break;
        default:
    return;
    }
}

bool ASIC_set_frequency(GlobalState * GLOBAL_STATE, float target_frequency) {
    ESP_LOGI(TAG, "Setting ASIC frequency to %.2f MHz", target_frequency);
    bool success = false;
    
    switch (GLOBAL_STATE->asic_model) {
        case ASIC_BM1370:
            success = BM1370_set_frequency(target_frequency);
            break;
        default:
            ESP_LOGE(TAG, "Unknown ASIC model, cannot set frequency");
            success = false;
            break;
    }
    
    if (success) {
        ESP_LOGI(TAG, "Successfully transitioned to new ASIC frequency: %.2f MHz", target_frequency);
    } else {
        ESP_LOGE(TAG, "Failed to transition to new ASIC frequency: %.2f MHz", target_frequency);
    }
    
    return success;
}

esp_err_t ASIC_set_device_model(GlobalState * GLOBAL_STATE) {

    if (GLOBAL_STATE->device_model_str == NULL) {
        ESP_LOGE(TAG, "No device model string found");
        return ESP_FAIL;
    }

    if (strcmp(GLOBAL_STATE->device_model_str, "BITFORGE_NANO") == 0) {
        GLOBAL_STATE->asic_model = ASIC_BM1370;
        float frequency_mhz = GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value;
        GLOBAL_STATE->asic_job_frequency_ms = calculate_bm_timeout_ms(
            frequency_mhz,
            BITFORGE_NANO_ASIC_COUNT,
            BM1370_SMALL_CORE_COUNT,
            BM1370_CORE_COUNT,
            BM1370_VERSION_ROLL_SIZE,
            BM1370_TIMEOUT_PERCENT,
            ASIC_BM1370_JOB_FREQUENCY_MS);
        GLOBAL_STATE->ASIC_difficulty = BM1370_ASIC_DIFFICULTY;
        ESP_LOGI(TAG, "DEVICE: BitForgeNano");
        ESP_LOGI(TAG, "ASIC: %dx BM1370 (%" PRIu64 " cores)", BITFORGE_NANO_ASIC_COUNT, BM1370_CORE_COUNT);
        ESP_LOGI(TAG, "Computed ASIC job interval: %.1f ms (freq=%.2f MHz, timeout_percent=%.2f)",
                 GLOBAL_STATE->asic_job_frequency_ms, frequency_mhz, BM1370_TIMEOUT_PERCENT);
        GLOBAL_STATE->device_model = BITFORGE_NANO;

    } else {
        ESP_LOGE(TAG, "Invalid DEVICE model");
        GLOBAL_STATE->device_model = DEVICE_UNKNOWN;
        return ESP_FAIL;
    }
    return ESP_OK;
}
