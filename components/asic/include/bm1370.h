#ifndef BM1370_H_
#define BM1370_H_

#include "common.h"
#include "driver/gpio.h"
#include "mining.h"

#define ASIC_BM1370_JOB_FREQUENCY_MS 500

#define BM1370_ASIC_DIFFICULTY 256

#define BM1370_VERSION_ROLL_SIZE 65536  // 2^16 (BIP320 version-rolling space)
#define BM1370_TIMEOUT_PERCENT   0.5f   // ~70s interval @ 490MHz, under stratum ~100s cap

#define BM1370_SERIALTX_DEBUG false
#define BM1370_SERIALRX_DEBUG false
#define BM1370_DEBUG_WORK false //causes insane amount of debug output
#define BM1370_DEBUG_JOBS false //causes insane amount of debug output

static const uint64_t BM1370_CORE_COUNT = 128;
static const uint64_t BM1370_SMALL_CORE_COUNT = 2040;

typedef struct
{
    float frequency;
} bm1370Module;

typedef struct __attribute__((__packed__))
{
    uint8_t job_id;
    uint8_t num_midstates;
    uint8_t starting_nonce[4];
    uint8_t nbits[4];
    uint8_t ntime[4];
    uint8_t merkle_root[32];
    uint8_t prev_block_hash[32];
    uint8_t version[4];
} BM1370_job;

uint8_t BM1370_init(uint64_t frequency, uint16_t asic_count);
void BM1370_send_work(void * GLOBAL_STATE, bm_job * next_bm_job);
void BM1370_set_job_difficulty_mask(int);
void BM1370_set_version_mask(uint32_t version_mask);
void BM1370_set_hash_counting_number(uint32_t hcn);
void BM1370_set_nonce_space(double nonce_percent, float frequency,
                            uint16_t asic_count, uint16_t cores);
int BM1370_set_max_baud(void);
int BM1370_set_default_baud(void);
void BM1370_send_hash_frequency(float frequency);
bool BM1370_set_frequency(float target_freq);
task_result * BM1370_process_work(void * GLOBAL_STATE);
void BM1370_read_registers(void);

#endif /* BM1370_H_ */
