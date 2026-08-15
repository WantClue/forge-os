#ifndef GLOBAL_STATE_H_
#define GLOBAL_STATE_H_

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "asic_task.h"
#include "bm1370.h"
#include "common.h"
#include "hashrate_monitor_task.h"
#include "power_management_task.h"
#include "serial.h"
#include "stratum_api.h"
#include "work_queue.h"
#include "esp_transport.h"

#define STRATUM_USER CONFIG_STRATUM_USER
#define FALLBACK_STRATUM_USER CONFIG_FALLBACK_STRATUM_USER

#define HISTORY_LENGTH 100
#define DIFF_STRING_SIZE 10
#define POOL_PRIMARY 0
#define POOL_SECONDARY 1
#define POOL_COUNT 2

typedef enum
{
    POOL_MODE_FALLBACK = 0,
    POOL_MODE_DUAL = 1,
} PoolMode;

typedef enum
{
    DEVICE_UNKNOWN = -1,
    BITFORGE_NANO,
} DeviceModel;

typedef enum
{
    ASIC_UNKNOWN = -1,
    ASIC_BM1370,
} AsicModel;

typedef enum
{
    MINING_STATE_MINING = 0,
    MINING_STATE_PAUSING,
    MINING_STATE_PAUSED,
    MINING_STATE_RESUMING,
} MiningState;

// typedef struct
// {
//     uint8_t (*init_fn)(uint64_t, uint16_t);
//     task_result * (*receive_result_fn)(void * GLOBAL_STATE);
//     int (*set_max_baud_fn)(void);
//     void (*set_difficulty_mask_fn)(int);
//     void (*send_work_fn)(void * GLOBAL_STATE, bm_job * next_bm_job);
//     void (*set_version_mask)(uint32_t);
// } AsicFunctions;

typedef struct {
    char message[64];
    uint32_t count;
} RejectedReasonStat;

typedef struct
{
    double duration_start;
    int historical_hashrate_rolling_index;
    double historical_hashrate_time_stamps[HISTORY_LENGTH];
    double historical_hashrate[HISTORY_LENGTH];
    int historical_hashrate_init;
    double current_hashrate;
    int64_t start_time;
    uint64_t shares_accepted;
    uint64_t shares_rejected;
    RejectedReasonStat rejected_reason_stats[10];
    int rejected_reason_stats_count;
    uint64_t best_nonce_diff;
    char best_diff_string[DIFF_STRING_SIZE];
    uint64_t best_session_nonce_diff;
    char best_session_diff_string[DIFF_STRING_SIZE];
    bool FOUND_BLOCK;
    char ssid[32];
    char wifi_status[20];
    char ip_addr_str[16]; // IP4ADDR_STRLEN_MAX
    char ap_ssid[32];
    bool ap_enabled;
    char * pool_url;
    char * fallback_pool_url;
    uint16_t pool_port;
    uint16_t fallback_pool_port;
    char * pool_user;
    char * fallback_pool_user;
    char * pool_pass;
    char * fallback_pool_pass;
    uint16_t pool_tls;
    uint16_t fallback_pool_tls;
    char * pool_cert;
    char * fallback_pool_cert;
    bool is_using_fallback;
    uint16_t pool_mode;
    uint16_t pool_balance;
    char pool_connection_info[64];
    uint16_t overheat_mode;
    volatile bool mining_paused;
    volatile MiningState mining_state;
    uint16_t power_fault;
    uint32_t lastClockSync;
    bool is_firmware_update;
    char firmware_update_filename[20];
    char firmware_update_status[20];
    char * asic_status;
} SystemModule;

typedef struct
{
    esp_transport_handle_t transport;
    portMUX_TYPE mux;
    int send_uid;
    int first_share_uid;
    bool connected;
    bool valid_notify;
    bool close_requested;
    char *extranonce_str;
    int extranonce_2_len;
    uint32_t version_mask;
    bool new_version_rolling_msg;
    double stratum_difficulty;
    mining_notify *current_notify;
    uint64_t extranonce_2;
    uint64_t shares_accepted;
    uint64_t shares_rejected;
} StratumPoolState;

typedef struct
{
    DeviceModel device_model;
    char * device_model_str;
    int board_version;
    AsicModel asic_model;
    char * asic_model_str;
    double asic_job_frequency_ms;
    uint32_t ASIC_difficulty;

    work_queue stratum_queue;
    work_queue ASIC_jobs_queue;

    StratumPoolState pools[POOL_COUNT];
    pthread_mutex_t stratum_work_lock;
    pthread_cond_t stratum_work_updated;
    int pool_error_accum;

    SystemModule SYSTEM_MODULE;
    AsicTaskModule ASIC_TASK_MODULE;
    PowerManagementModule POWER_MANAGEMENT_MODULE;
    HashrateMonitorModule HASHRATE_MONITOR_MODULE;

    char * extranonce_str;
    int extranonce_2_len;
    int abandon_work;

    uint8_t * valid_jobs;
    pthread_mutex_t valid_jobs_lock;

    double stratum_difficulty;
    uint32_t version_mask;
    bool new_stratum_version_rolling_msg;

    esp_transport_handle_t transport;

    // A message ID that must be unique per request that expects a response.
    // For requests not expecting a response (called notifications), this is null.
    int send_uid;

    // Guards send_uid and the transport pointer against the stratum/asic_result
    // tasks racing on share submit vs. connection teardown.
    portMUX_TYPE stratum_mux;

    bool ASIC_initalized;
    bool mining_control_ready;
    TaskHandle_t power_management_task_handle;
    bool psram_is_available;
} GlobalState;

#endif /* GLOBAL_STATE_H_ */
