#include <sys/time.h>
#include <limits.h>

#include "work_queue.h"
#include "global_state.h"
#include "esp_log.h"
#include "esp_system.h"
#include "mining.h"
#include "string.h"

#include "asic.h"

static const char *TAG = "create_jobs_task";

#define QUEUE_LOW_WATER_MARK 1

static bool should_generate_more_work(GlobalState *GLOBAL_STATE);
static void generate_work(GlobalState *GLOBAL_STATE, mining_notify *notification, uint64_t extranonce_2,
                          const char *extranonce_str, int extranonce_2_len, uint32_t version_mask, int pool_id);
static void create_jobs_dual_pool_task(GlobalState *GLOBAL_STATE);
static mining_notify *clone_mining_notify(const mining_notify *notification);
static int dual_pool_get_next_active_pool_locked(GlobalState *GLOBAL_STATE);

void create_jobs_task(void *pvParameters)
{
    GlobalState *GLOBAL_STATE = (GlobalState *)pvParameters;

    if (GLOBAL_STATE->SYSTEM_MODULE.pool_mode == POOL_MODE_DUAL) {
        create_jobs_dual_pool_task(GLOBAL_STATE);
        vTaskDelete(NULL);
        return;
    }

    while (1)
    {
        mining_notify *mining_notification = (mining_notify *)queue_dequeue(&GLOBAL_STATE->stratum_queue);
        if (mining_notification == NULL) {
            ESP_LOGE(TAG, "Failed to dequeue mining notification");
            vTaskDelay(100 / portTICK_PERIOD_MS); // Wait a bit before trying again
            continue;
        }

        ESP_LOGI(TAG, "New Work Dequeued %s", mining_notification->job_id);

        if (GLOBAL_STATE->abandon_work == 1)
        {
            GLOBAL_STATE->abandon_work = 0;
            pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock);
            ASIC_jobs_queue_clear(&GLOBAL_STATE->ASIC_jobs_queue);
            pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);
            xSemaphoreGive(GLOBAL_STATE->ASIC_TASK_MODULE.semaphore);
        }

        if (GLOBAL_STATE->new_stratum_version_rolling_msg) {
            ESP_LOGI(TAG, "Set chip version rolls %i", (int)(GLOBAL_STATE->version_mask >> 13));
            //(GLOBAL_STATE->ASIC_functions.set_version_mask)(GLOBAL_STATE->version_mask);
            ASIC_set_version_mask(GLOBAL_STATE, GLOBAL_STATE->version_mask);
            GLOBAL_STATE->new_stratum_version_rolling_msg = false;
        }

        uint64_t extranonce_2 = 0;
        while (queue_count(&GLOBAL_STATE->stratum_queue) < 1 &&
               GLOBAL_STATE->abandon_work == 0 &&
               !GLOBAL_STATE->SYSTEM_MODULE.mining_paused)
        {
            if (should_generate_more_work(GLOBAL_STATE))
            {
                generate_work(GLOBAL_STATE, mining_notification, extranonce_2,
                              GLOBAL_STATE->extranonce_str, GLOBAL_STATE->extranonce_2_len,
                              GLOBAL_STATE->version_mask,
                              GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? POOL_SECONDARY : POOL_PRIMARY);

                // Increase extranonce_2 for the next job.
                extranonce_2++;
            }
            else
            {
                // If no more work needed, wait a bit before checking again.
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
        }

        if (GLOBAL_STATE->abandon_work == 1)
        {
            GLOBAL_STATE->abandon_work = 0;
            pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock);
            ASIC_jobs_queue_clear(&GLOBAL_STATE->ASIC_jobs_queue);
            pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);
            xSemaphoreGive(GLOBAL_STATE->ASIC_TASK_MODULE.semaphore);
        }

        STRATUM_V1_free_mining_notify(mining_notification);
    }
}

static bool should_generate_more_work(GlobalState *GLOBAL_STATE)
{
    return queue_count(&GLOBAL_STATE->ASIC_jobs_queue) < QUEUE_LOW_WATER_MARK;
}

static bool dual_pool_has_valid_work(GlobalState *GLOBAL_STATE)
{
    return GLOBAL_STATE->pools[POOL_PRIMARY].valid_notify || GLOBAL_STATE->pools[POOL_SECONDARY].valid_notify;
}

static int dual_pool_get_next_active_pool_locked(GlobalState *GLOBAL_STATE)
{
    int secondary_pct = 100 - GLOBAL_STATE->SYSTEM_MODULE.pool_balance;
    bool valid0 = GLOBAL_STATE->pools[POOL_PRIMARY].valid_notify;
    bool valid1 = GLOBAL_STATE->pools[POOL_SECONDARY].valid_notify;

    if (valid0 && !valid1) {
        GLOBAL_STATE->pool_error_accum = 0;
        return POOL_PRIMARY;
    }
    if (!valid0 && valid1) {
        GLOBAL_STATE->pool_error_accum = 0;
        return POOL_SECONDARY;
    }
    if (!valid0 && !valid1) {
        GLOBAL_STATE->pool_error_accum = 0;
        return POOL_PRIMARY;
    }

    GLOBAL_STATE->pool_error_accum += secondary_pct;
    if (GLOBAL_STATE->pool_error_accum >= 100) {
        GLOBAL_STATE->pool_error_accum -= 100;
        return POOL_SECONDARY;
    }
    return POOL_PRIMARY;
}

static mining_notify *clone_mining_notify(const mining_notify *notification)
{
    if (notification == NULL) {
        return NULL;
    }

    mining_notify *clone = calloc(1, sizeof(mining_notify));
    if (clone == NULL) {
        return NULL;
    }

    clone->job_id = notification->job_id ? strdup(notification->job_id) : NULL;
    clone->prev_block_hash = notification->prev_block_hash ? strdup(notification->prev_block_hash) : NULL;
    clone->coinbase_1 = notification->coinbase_1 ? strdup(notification->coinbase_1) : NULL;
    clone->coinbase_2 = notification->coinbase_2 ? strdup(notification->coinbase_2) : NULL;
    clone->n_merkle_branches = notification->n_merkle_branches;
    if (notification->n_merkle_branches > 0) {
        size_t branches_size = notification->n_merkle_branches * 32;
        clone->merkle_branches = malloc(branches_size);
        if (clone->merkle_branches != NULL) {
            memcpy(clone->merkle_branches, notification->merkle_branches, branches_size);
        }
    }
    clone->version = notification->version;
    clone->target = notification->target;
    clone->ntime = notification->ntime;
    clone->difficulty = notification->difficulty;
    clone->clean_jobs = notification->clean_jobs;

    if ((notification->job_id && clone->job_id == NULL) ||
        (notification->prev_block_hash && clone->prev_block_hash == NULL) ||
        (notification->coinbase_1 && clone->coinbase_1 == NULL) ||
        (notification->coinbase_2 && clone->coinbase_2 == NULL) ||
        (notification->n_merkle_branches > 0 && clone->merkle_branches == NULL)) {
        STRATUM_V1_free_mining_notify(clone);
        return NULL;
    }

    return clone;
}

static void create_jobs_dual_pool_task(GlobalState *GLOBAL_STATE)
{
    while (1) {
        if (!should_generate_more_work(GLOBAL_STATE)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        pthread_mutex_lock(&GLOBAL_STATE->stratum_work_lock);
        while (!dual_pool_has_valid_work(GLOBAL_STATE)) {
            pthread_cond_wait(&GLOBAL_STATE->stratum_work_updated, &GLOBAL_STATE->stratum_work_lock);
        }

        int pool_id = dual_pool_get_next_active_pool_locked(GLOBAL_STATE);
        StratumPoolState *pool = &GLOBAL_STATE->pools[pool_id];
        mining_notify *notification = clone_mining_notify(pool->current_notify);
        uint64_t extranonce_2 = pool->extranonce_2++;
        char *extranonce_str = pool->extranonce_str ? strdup(pool->extranonce_str) : NULL;
        int extranonce_2_len = pool->extranonce_2_len;
        uint32_t version_mask = pool->version_mask;
        pthread_mutex_unlock(&GLOBAL_STATE->stratum_work_lock);

        if (notification == NULL || extranonce_str == NULL || extranonce_2_len <= 0) {
            ESP_LOGW(TAG, "Selected pool %d does not have complete work data", pool_id);
            if (notification != NULL) {
                STRATUM_V1_free_mining_notify(notification);
            }
            free(extranonce_str);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        generate_work(GLOBAL_STATE, notification, extranonce_2, extranonce_str, extranonce_2_len, version_mask, pool_id);
        STRATUM_V1_free_mining_notify(notification);
        free(extranonce_str);
    }
}

static void generate_work(GlobalState *GLOBAL_STATE, mining_notify *notification, uint64_t extranonce_2,
                          const char *extranonce_str, int extranonce_2_len, uint32_t version_mask, int pool_id)
{
    char *extranonce_2_str = extranonce_2_generate(extranonce_2, extranonce_2_len);
    if (extranonce_2_str == NULL) {
        ESP_LOGE(TAG, "Failed to generate extranonce_2");
        return;
    }

    char *coinbase_tx = construct_coinbase_tx(notification->coinbase_1, notification->coinbase_2, extranonce_str, extranonce_2_str);
    if (coinbase_tx == NULL) {
        ESP_LOGE(TAG, "Failed to construct coinbase_tx");
        free(extranonce_2_str);
        return;
    }

    char *merkle_root = calculate_merkle_root_hash(coinbase_tx, (uint8_t(*)[32])notification->merkle_branches, notification->n_merkle_branches);
    if (merkle_root == NULL) {
        ESP_LOGE(TAG, "Failed to calculate merkle_root");
        free(extranonce_2_str);
        free(coinbase_tx);
        return;
    }

    bm_job next_job = construct_bm_job(notification, merkle_root, version_mask);

    bm_job *queued_next_job = malloc(sizeof(bm_job));
    if (queued_next_job == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for queued_next_job");
        free(extranonce_2_str);
        free(coinbase_tx);
        free(merkle_root);
        return;
    }

    memcpy(queued_next_job, &next_job, sizeof(bm_job));
    queued_next_job->extranonce2 = extranonce_2_str; // Transfer ownership
    queued_next_job->jobid = strdup(notification->job_id);
    queued_next_job->version_mask = version_mask;
    queued_next_job->pool_id = pool_id;
    queued_next_job->asic_diff = GLOBAL_STATE->ASIC_difficulty;

    queue_enqueue(&GLOBAL_STATE->ASIC_jobs_queue, queued_next_job);

    free(coinbase_tx);
    free(merkle_root);
}
