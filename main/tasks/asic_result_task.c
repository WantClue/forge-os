#include <lwip/tcpip.h>

#include "system.h"
#include "work_queue.h"
#include "serial.h"
#include <string.h>
#include "esp_log.h"
#include "nvs_config.h"
#include "utils.h"
#include "stratum_task.h"
#include "asic.h"
#include "hashrate_monitor_task.h"

static const char *TAG = "asic_result";

void ASIC_result_task(void *pvParameters)
{
    GlobalState *GLOBAL_STATE = (GlobalState *)pvParameters;

    while (1)
    {
        //task_result *asic_result = (*GLOBAL_STATE->ASIC_functions.receive_result_fn)(GLOBAL_STATE);
        task_result *asic_result = ASIC_process_work(GLOBAL_STATE);

        if (asic_result == NULL)
        {
            continue;
        }

        // Check if this is a register response
        if (asic_result->register_type != REGISTER_INVALID)
        {
            ESP_LOGD(TAG, "Register response detected: type=%d, asic=%d, value=0x%08X",
                     asic_result->register_type, asic_result->asic_nr, asic_result->value);
            // Call hashrate monitor callback
            if (GLOBAL_STATE->HASHRATE_MONITOR_MODULE.is_initialized)
            {
                hashrate_monitor_register_read(GLOBAL_STATE, asic_result->register_type, asic_result->asic_nr, asic_result->value);
            } else {
                ESP_LOGW(TAG, "Hashrate monitor not initialized yet");
            }
            continue;
        }

        uint8_t job_id = asic_result->job_id;

        pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock);

        if (GLOBAL_STATE->valid_jobs[job_id] == 0)
        {
            pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);
            ESP_LOGW(TAG, "Invalid job nonce found, 0x%02X", job_id);
            continue;
        }

        bm_job *active_job = GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id];

        // check the nonce difficulty
        double nonce_diff = test_nonce_value(
            active_job,
            asic_result->nonce,
            asic_result->rolled_version);

        //log the ASIC response
        ESP_LOGD(TAG, "Ver: %08" PRIX32 " Nonce %08" PRIX32 " diff %.1f of %.2f.", asic_result->rolled_version, asic_result->nonce, nonce_diff, active_job->pool_diff);

        SYSTEM_notify_found_nonce(GLOBAL_STATE, nonce_diff, job_id);

        if (nonce_diff >= active_job->pool_diff)
        {
            int pool_id = active_job->pool_id;
            char *user = pool_id == POOL_SECONDARY ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_user : GLOBAL_STATE->SYSTEM_MODULE.pool_user;
            char *jobid = strdup(active_job->jobid);
            char *extranonce2 = strdup(active_job->extranonce2);
            uint32_t ntime = active_job->ntime;
            uint32_t version_bits = asic_result->rolled_version ^ active_job->version;

            if (jobid == NULL || extranonce2 == NULL) {
                free(jobid);
                free(extranonce2);
                pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);
                ESP_LOGE(TAG, "Failed to copy share data");
                continue;
            }

            // Snapshot transport + uid atomically so stratum_close_connection can't
            // destroy the transport between the read here and the write below.
            esp_transport_handle_t transport = NULL;
            int uid = 0;
            if (GLOBAL_STATE->SYSTEM_MODULE.pool_mode == POOL_MODE_DUAL) {
                StratumPoolState *pool = &GLOBAL_STATE->pools[pool_id];
                taskENTER_CRITICAL(&pool->mux);
                transport = pool->transport;
                uid = pool->send_uid++;
                taskEXIT_CRITICAL(&pool->mux);
            } else {
                taskENTER_CRITICAL(&GLOBAL_STATE->stratum_mux);
                transport = GLOBAL_STATE->transport;
                uid = GLOBAL_STATE->send_uid++;
                taskEXIT_CRITICAL(&GLOBAL_STATE->stratum_mux);
            }

            pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);

            int ret = 0;
            if (transport == NULL) {
                ESP_LOGW(TAG, "No transport available, dropping share");
            } else {
                ret = STRATUM_V1_submit_share(
                    transport,
                    uid,
                    user,
                    jobid,
                    extranonce2,
                    ntime,
                    asic_result->nonce,
                    version_bits);

                if (ret < 0) {
                    ESP_LOGI(TAG, "Unable to write share to socket. Closing connection. Ret: %d (errno %d: %s)", ret, errno, strerror(errno));
                    if (GLOBAL_STATE->SYSTEM_MODULE.pool_mode == POOL_MODE_DUAL) {
                        stratum_close_pool_connection(GLOBAL_STATE, pool_id);
                    } else {
                        stratum_close_connection(GLOBAL_STATE);
                    }
                }
            }
            free(jobid);
            free(extranonce2);
        } else {
            pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);
        }
    }
}
