#ifndef HASHRATE_MONITOR_TASK_H_
#define HASHRATE_MONITOR_TASK_H_

#include "common.h"
#include <pthread.h>

typedef struct {
    uint32_t value;
    uint32_t time_ms;
    float hashrate;
} measurement_t;

typedef struct {
    measurement_t* total_measurement;
    measurement_t** domain_measurements;
    measurement_t* error_measurement;

    float hashrate;
    int error_count;
    bool is_initialized;
    pthread_mutex_t measurement_lock;
} HashrateMonitorModule;

void hashrate_monitor_task(void *pvParameters);
void hashrate_monitor_register_read(void *pvParameters, register_type_t register_type, uint8_t asic_nr, uint32_t value);
void hashrate_monitor_reset(void *pvParameters);
float hash_counter_to_ghs(uint32_t duration_ms, uint32_t counter);

#endif /* HASHRATE_MONITOR_TASK_H_ */
