#ifndef STRATUM_TASK_H_
#define STRATUM_TASK_H_

typedef struct
{
    double stratum_difficulty;
} SystemTaskModule;

// Returned by stratum_submit_share() when the pool is not currently connected.
#define STRATUM_SUBMIT_NO_CONNECTION (-1000)

void stratum_task(void *pvParameters);

// Worker-only. Closing a transport from any task other than the one blocked in
// its read is a use-after-free; other tasks must use the request_* variants,
// which only unblock the worker and leave the destroying to it.
void stratum_close_connection(GlobalState * GLOBAL_STATE);
void stratum_close_pool_connection(GlobalState * GLOBAL_STATE, int pool_id);

// Safe from any task.
void stratum_request_close(GlobalState * GLOBAL_STATE);
void stratum_request_pool_close(GlobalState * GLOBAL_STATE, int pool_id);
int stratum_submit_share(GlobalState * GLOBAL_STATE, int pool_id, const char *username,
                         const char *job_id, const char *extranonce_2, uint32_t ntime,
                         uint32_t nonce, uint32_t version_bits);

int stratum_get_next_uid(GlobalState * GLOBAL_STATE);
int stratum_get_next_pool_uid(GlobalState * GLOBAL_STATE, int pool_id);

#endif
