#ifndef SYSTEM_H_
#define SYSTEM_H_

#include "esp_err.h"
#include "global_state.h"

typedef enum {
    BLOCK_STATUS_NONE = 0,      // never found a block
    BLOCK_STATUS_PENDING = 1,   // found, waiting for the pool to build on it
    BLOCK_STATUS_CONFIRMED = 2, // pool sent a job whose prev hash is our block
    BLOCK_STATUS_ORPHANED = 3,  // chain moved on to a different block
} block_status_t;

typedef struct {
    uint64_t count;
    block_status_t status;
    uint32_t time;
    char hash[65]; // display order (as shown by block explorers), "" if none
} system_block_info_t;

void SYSTEM_init_system(GlobalState * GLOBAL_STATE);
esp_err_t SYSTEM_init_peripherals(GlobalState * GLOBAL_STATE);

esp_err_t SYSTEM_set_led_blink_enabled(GlobalState * GLOBAL_STATE, bool enabled);
bool SYSTEM_get_led_blink_enabled(GlobalState * GLOBAL_STATE);

void SYSTEM_notify_accepted_share(GlobalState * GLOBAL_STATE, int pool_id);
void SYSTEM_notify_rejected_share(GlobalState * GLOBAL_STATE, int pool_id, char * error_msg);
// hash is the header's double-SHA256 (internal byte order). Must be called
// with valid_jobs_lock held so active_jobs[job_id] stays valid.
void SYSTEM_notify_found_nonce(GlobalState * GLOBAL_STATE, double found_diff, uint8_t job_id, const uint8_t hash[32]);
// Called for every new job; prev_hash is the job's prev_block_hash in header
// byte order. Resolves a pending block to confirmed or orphaned.
void SYSTEM_notify_new_prev_hash(GlobalState * GLOBAL_STATE, const uint8_t prev_hash[32]);
void SYSTEM_get_block_info(GlobalState * GLOBAL_STATE, system_block_info_t * out);
// Forget all found-block data (count, last hash, status), in RAM and NVS.
void SYSTEM_clear_block_info(GlobalState * GLOBAL_STATE);
const char * SYSTEM_block_status_string(block_status_t status);
void SYSTEM_notify_new_ntime(GlobalState * GLOBAL_STATE, uint32_t ntime);

#endif /* SYSTEM_H_ */
