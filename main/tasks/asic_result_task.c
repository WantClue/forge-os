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

        // check the nonce difficulty
        double nonce_diff = test_nonce_value(
            GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id],
            asic_result->nonce,
            asic_result->rolled_version);

        //log the ASIC response
        ESP_LOGD(TAG, "Ver: %08" PRIX32 " Nonce %08" PRIX32 " diff %.1f of %ld.", asic_result->rolled_version, asic_result->nonce, nonce_diff, GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]->pool_diff);

        if (nonce_diff >= GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]->pool_diff)
        {
            char * user = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_user : GLOBAL_STATE->SYSTEM_MODULE.pool_user;
            int ret = STRATUM_V1_submit_share(
                GLOBAL_STATE->transport,
                GLOBAL_STATE->send_uid++,
                user,
                GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]->jobid,
                GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]->extranonce2,
                GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]->ntime,
                asic_result->nonce,
                asic_result->rolled_version ^ GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]->version);

            pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);

            if (ret < 0) {
                ESP_LOGI(TAG, "Unable to write share to socket. Closing connection. Ret: %d (errno %d: %s)", ret, errno, strerror(errno));
                stratum_close_connection(GLOBAL_STATE);
            }
        } else {
            pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);
        }

        SYSTEM_notify_found_nonce(GLOBAL_STATE, nonce_diff, job_id);
    }
}
