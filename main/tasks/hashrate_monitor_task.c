#include <string.h>
#include <inttypes.h>
#include <esp_heap_caps.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "common.h"
#include "bm1370.h"
#include "hashrate_monitor_task.h"
#include "global_state.h"
#include "asic.h"

#define POLL_RATE 5000
#define EMA_ALPHA 12

#define HASH_CNT_LSB 0x100000000uLL // Hash counters are incremented on difficulty 1 (2^32 hashes)
#define HASHRATE_UNIT 0x100000uLL // Hashrate register unit (2^24 hashes)

static const char *TAG = "hashrate_monitor";

static float frequency_value;

static float sum_hashrates(measurement_t * measurement, int asic_count)
{
    if (asic_count == 1) return measurement[0].hashrate;

    float total = 0;
    int responding_asics = 0;
    for (int i = 0; i < asic_count; i++) {
        if (measurement[i].hashrate > 0.0) {
            total += measurement[i].hashrate;
            responding_asics++;
        }
    }
    // Only return 0 if NO ASICs are responding
    return total;
}

static uint32_t sum_values(measurement_t * measurement, int asic_count)
{
    if (asic_count == 1) return measurement[0].value;

    uint32_t total = 0;
    for (int i = 0; i < asic_count; i++) total += measurement[i].value;
    return total;
}

static void clear_measurements(HashrateMonitorModule * HASHRATE_MONITOR_MODULE, int asic_count, int hash_domains)
{
    memset(HASHRATE_MONITOR_MODULE->total_measurement, 0, asic_count * sizeof(measurement_t));
    if (hash_domains > 0) {
        memset(HASHRATE_MONITOR_MODULE->domain_measurements[0], 0, asic_count * hash_domains * sizeof(measurement_t));
    }
    memset(HASHRATE_MONITOR_MODULE->error_measurement, 0, asic_count * sizeof(measurement_t));
}

float hash_counter_to_ghs(uint32_t duration_ms, uint32_t counter)
{
    if (duration_ms == 0) return 0.0f;
    float seconds = duration_ms / 1000.0;
    float hashrate = counter / seconds * (float)HASH_CNT_LSB; // Make sure it stays in float
    return hashrate / 1e9f; // Convert to Gh/s
}

static void update_hashrate(uint32_t value, measurement_t * measurement, int asic_nr)
{
    uint8_t flag_long = (value & 0x80000000) >> 31;
    uint32_t hashrate_value = value & 0x7FFFFFFF;

    if (hashrate_value != 0x007FFFFF && !flag_long) {
        float hashrate = hashrate_value * (float)HASHRATE_UNIT; // Make sure it stays in float
        measurement[asic_nr].hashrate =  hashrate / 1e9f; // Convert to Gh/s
    }
}

static void update_hash_counter(uint32_t time_ms, uint32_t value, measurement_t * measurement, int asic_nr)
{
    uint32_t previous_time_ms = measurement[asic_nr].time_ms;
    if (previous_time_ms != 0) {
        uint32_t duration_ms = time_ms - previous_time_ms;
        uint32_t counter = value - measurement[asic_nr].value; // Compute counter difference, handling uint32_t wraparound
        measurement[asic_nr].hashrate = hash_counter_to_ghs(duration_ms, counter);
        ESP_LOGD(TAG, "ASIC %d: counter delta=%"PRIu32", duration=%"PRIu32"ms, hashrate=%.2f GH/s",
                 asic_nr, counter, duration_ms, measurement[asic_nr].hashrate);
    } else {
        ESP_LOGD(TAG, "ASIC %d: First measurement, storing value=0x%08"PRIX32", time=%"PRIu32,
                 asic_nr, value, time_ms);
    }

    measurement[asic_nr].value = value;
    measurement[asic_nr].time_ms = time_ms;
}

void hashrate_monitor_task(void *pvParameters)
{
    GlobalState * GLOBAL_STATE = (GlobalState *)pvParameters;
    HashrateMonitorModule * HASHRATE_MONITOR_MODULE = &GLOBAL_STATE->HASHRATE_MONITOR_MODULE;
    int asic_count = ASIC_get_asic_count(GLOBAL_STATE);
    int hash_domains = BM1370_HASH_DOMAINS;

    HASHRATE_MONITOR_MODULE->total_measurement = heap_caps_malloc(asic_count * sizeof(measurement_t), MALLOC_CAP_SPIRAM);
    if (hash_domains > 0) {
        measurement_t* data = malloc(asic_count * hash_domains * sizeof(measurement_t));
        HASHRATE_MONITOR_MODULE->domain_measurements = heap_caps_malloc(hash_domains * sizeof(measurement_t*), MALLOC_CAP_SPIRAM);
        for (size_t i = 0; i < hash_domains; i++) {
            HASHRATE_MONITOR_MODULE->domain_measurements[i] = data + (i * asic_count);
        }
    }
    HASHRATE_MONITOR_MODULE->error_measurement = heap_caps_malloc(asic_count * sizeof(measurement_t), MALLOC_CAP_SPIRAM);

    clear_measurements(HASHRATE_MONITOR_MODULE, asic_count, hash_domains);

    HASHRATE_MONITOR_MODULE->is_initialized = true;
    ESP_LOGI(TAG, "Hashrate monitor initialized for %d ASICs", asic_count);

    TickType_t taskWakeTime = xTaskGetTickCount();
    while (1) {
        BM1370_read_registers();
        ESP_LOGI(TAG, "Sent register read commands");

        vTaskDelay(100 / portTICK_PERIOD_MS);

        float hashrate = sum_hashrates(HASHRATE_MONITOR_MODULE->total_measurement, asic_count);
        ESP_LOGI(TAG, "Calculated hashrate: %.2f GH/s", hashrate);

        if (hashrate == 0.0) {
            HASHRATE_MONITOR_MODULE->hashrate = 0.0;
        } else {
            if (HASHRATE_MONITOR_MODULE->hashrate == 0.0f) {
                // Initialize with current hashrate
                HASHRATE_MONITOR_MODULE->hashrate = hashrate;
                ESP_LOGI(TAG, "Initial hashrate set to %.2f GH/s", hashrate);
            } else {
                HASHRATE_MONITOR_MODULE->hashrate = ((HASHRATE_MONITOR_MODULE->hashrate * (EMA_ALPHA - 1)) + hashrate) / EMA_ALPHA;
                ESP_LOGI(TAG, "EMA hashrate: %.2f GH/s", HASHRATE_MONITOR_MODULE->hashrate);
            }
        }

        float error_hashrate = sum_hashrates(HASHRATE_MONITOR_MODULE->error_measurement, asic_count);
        HASHRATE_MONITOR_MODULE->error_percentage = HASHRATE_MONITOR_MODULE->hashrate > 0
            ? error_hashrate / HASHRATE_MONITOR_MODULE->hashrate * 100.0f
            : 0.0f;

        HASHRATE_MONITOR_MODULE->error_count = sum_values(HASHRATE_MONITOR_MODULE->error_measurement, asic_count);

        vTaskDelayUntil(&taskWakeTime, POLL_RATE / portTICK_PERIOD_MS);
    }
}

void hashrate_monitor_register_read(void *pvParameters, register_type_t register_type, uint8_t asic_nr, uint32_t value)
{
    uint32_t time_ms = esp_timer_get_time() / 1000;

    GlobalState * GLOBAL_STATE = (GlobalState *)pvParameters;
    HashrateMonitorModule * HASHRATE_MONITOR_MODULE = &GLOBAL_STATE->HASHRATE_MONITOR_MODULE;
    PowerManagementModule * POWER_MANAGEMENT_MODULE = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;

    int asic_count = ASIC_get_asic_count(GLOBAL_STATE);
    int hash_domains = BM1370_HASH_DOMAINS;

    ESP_LOGD(TAG, "Register read: type=%d, asic=%d, value=0x%08"PRIX32, register_type, asic_nr, value);

    if (asic_nr >= asic_count) {
        ESP_LOGE(TAG, "Asic nr out of bounds");
        return;
    }

    // Reset statistics on start and when frequency changes
    if (POWER_MANAGEMENT_MODULE->frequency_value != frequency_value) {
        clear_measurements(HASHRATE_MONITOR_MODULE, asic_count, hash_domains);
        frequency_value = POWER_MANAGEMENT_MODULE->frequency_value;
    }

    switch(register_type) {
        case REGISTER_HASHRATE:
            update_hashrate(value, HASHRATE_MONITOR_MODULE->total_measurement, asic_nr);
            break;
        case REGISTER_TOTAL_COUNT:
            update_hash_counter(time_ms, value, HASHRATE_MONITOR_MODULE->total_measurement, asic_nr);
            break;
        case REGISTER_DOMAIN_0_COUNT:
            update_hash_counter(time_ms, value, HASHRATE_MONITOR_MODULE->domain_measurements[0], asic_nr);
            break;
        case REGISTER_DOMAIN_1_COUNT:
            update_hash_counter(time_ms, value, HASHRATE_MONITOR_MODULE->domain_measurements[1], asic_nr);
            break;
        case REGISTER_DOMAIN_2_COUNT:
            update_hash_counter(time_ms, value, HASHRATE_MONITOR_MODULE->domain_measurements[2], asic_nr);
            break;
        case REGISTER_DOMAIN_3_COUNT:
            update_hash_counter(time_ms, value, HASHRATE_MONITOR_MODULE->domain_measurements[3], asic_nr);
            break;
        case REGISTER_ERROR_COUNT:
            update_hash_counter(time_ms, value, HASHRATE_MONITOR_MODULE->error_measurement, asic_nr);
            break;
        case REGISTER_INVALID:
            ESP_LOGE(TAG, "Invalid register type");
            break;
    }
}
