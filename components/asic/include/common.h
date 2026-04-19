#ifndef COMMON_H_
#define COMMON_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

static const double NONCE_SPACE = 4294967296.0; // 2^32

typedef enum
{
    REGISTER_INVALID = 0,
    REGISTER_HASHRATE,       // hashrate register
    REGISTER_TOTAL_COUNT,    // total counter
    REGISTER_DOMAIN_0_COUNT, // domain counters
    REGISTER_DOMAIN_1_COUNT,
    REGISTER_DOMAIN_2_COUNT,
    REGISTER_DOMAIN_3_COUNT,
    REGISTER_ERROR_COUNT,    // error count register (all)
} register_type_t;

typedef struct __attribute__((__packed__))
{
    // -- job result response
    uint8_t job_id;
    uint32_t nonce;
    uint32_t rolled_version;
    // ---- register response
    register_type_t register_type;
    uint8_t asic_nr;
    uint32_t value;
} task_result;

unsigned char _reverse_bits(unsigned char num);
int _largest_power_of_two(int num);
int _next_power_of_two(int num);

int count_asic_chips(uint16_t asic_count, uint16_t chip_id, int chip_id_response_length);
esp_err_t receive_work(uint8_t * buffer, int buffer_size);

double calculate_bm_timeout_ms(float frequency_mhz, size_t asic_count,
                               size_t small_cores, size_t cores,
                               size_t version_size, float timeout_percent,
                               double default_time_ms);

#endif /* COMMON_H_ */
