#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet.h"

#include "system.h"
#include "INA260.h"
#include "adc.h"
#include "connect.h"
#include "nvs_config.h"
#include "vcore.h"
#include "ThermalMonitoring.h"
#include "sv2_protocol.h"
#include "mining.h"
#include "utils.h"

static const char * TAG = "SystemModule";

static void _suffix_string(uint64_t, char *, size_t, int);

#define BLINK_GPIO_1 9
#define BLINK_GPIO_2 12

static esp_netif_t * netif;
static TimerHandle_t led_timer_1 = NULL;
static TimerHandle_t led_timer_2 = NULL;
static pthread_mutex_t led_blink_lock = PTHREAD_MUTEX_INITIALIZER;
// Guards the last_block_* fields, written by the ASIC result and job tasks
// and read by the HTTP server.
static pthread_mutex_t block_lock = PTHREAD_MUTEX_INITIALIZER;

//local function prototypes
static esp_err_t ensure_overheat_mode_config();

static void _check_for_best_diff(GlobalState * GLOBAL_STATE, double diff, uint8_t job_id);
static void _load_block_state(SystemModule * module);
static void _suffix_string(uint64_t val, char * buf, size_t bufsiz, int sigdigits);

void SYSTEM_init_system(GlobalState * GLOBAL_STATE)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    module->shares_accepted = 0;
    module->shares_rejected = 0;
    module->led_blink_enabled = nvs_config_get_u16(NVS_CONFIG_LED_BLINK, 1) != 0;
    module->best_nonce_diff = nvs_config_get_u64(NVS_CONFIG_BEST_DIFF, 0);
    module->best_session_nonce_diff = 0;
    module->start_time = esp_timer_get_time();
    module->lastClockSync = 0;
    _load_block_state(module);

    GLOBAL_STATE->stratum_mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    GLOBAL_STATE->send_uid = 1;
    GLOBAL_STATE->transport = NULL;
    GLOBAL_STATE->transport_fd = -1;
    GLOBAL_STATE->close_requested = false;
    pthread_mutex_init(&GLOBAL_STATE->transport_lock, NULL);
    pthread_mutex_init(&GLOBAL_STATE->stratum_work_lock, NULL);
    pthread_cond_init(&GLOBAL_STATE->stratum_work_updated, NULL);
    pthread_mutex_init(&GLOBAL_STATE->valid_jobs_lock, NULL);
    pthread_mutex_init(&GLOBAL_STATE->coinbase_lock, NULL);
    GLOBAL_STATE->pool_error_accum = 0;

    // set the pool url
    module->pool_url = nvs_config_get_string(NVS_CONFIG_STRATUM_URL, CONFIG_STRATUM_URL);
    module->fallback_pool_url = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_URL, CONFIG_FALLBACK_STRATUM_URL);

    // set the pool port
    module->pool_port = nvs_config_get_u16(NVS_CONFIG_STRATUM_PORT, CONFIG_STRATUM_PORT);
    module->fallback_pool_port = nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_PORT, CONFIG_FALLBACK_STRATUM_PORT);

    // set the pool user
    module->pool_user = nvs_config_get_string(NVS_CONFIG_STRATUM_USER, CONFIG_STRATUM_USER);
    module->fallback_pool_user = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_USER, CONFIG_FALLBACK_STRATUM_USER);

    // set the pool password
    module->pool_pass = nvs_config_get_string(NVS_CONFIG_STRATUM_PASS, CONFIG_STRATUM_PW);
    module->fallback_pool_pass = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_PASS, CONFIG_FALLBACK_STRATUM_PW);

    // set the pool tls
    module->pool_tls = nvs_config_get_u16(NVS_CONFIG_STRATUM_TLS, CONFIG_STRATUM_TLS);
    module->fallback_pool_tls = nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_TLS, CONFIG_FALLBACK_STRATUM_TLS);

    // set the pool cert
    module->pool_cert = nvs_config_get_string(NVS_CONFIG_STRATUM_CERT, CONFIG_STRATUM_CERT);
    module->fallback_pool_cert = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_CERT, CONFIG_FALLBACK_STRATUM_CERT);

    // set the pool protocol and its Stratum V2 options
    module->pool_protocol = nvs_config_get_u16(NVS_CONFIG_STRATUM_PROTOCOL, STRATUM_PROTOCOL_V1);
    module->fallback_pool_protocol = nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_PROTOCOL, STRATUM_PROTOCOL_V1);
    if (module->pool_protocol > STRATUM_PROTOCOL_V2) {
        module->pool_protocol = STRATUM_PROTOCOL_V1;
    }
    if (module->fallback_pool_protocol > STRATUM_PROTOCOL_V2) {
        module->fallback_pool_protocol = STRATUM_PROTOCOL_V1;
    }
    module->pool_sv2_channel_type = nvs_config_get_u16(NVS_CONFIG_STRATUM_SV2_CHANNEL, SV2_CHANNEL_EXTENDED);
    module->fallback_pool_sv2_channel_type = nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_SV2_CHANNEL, SV2_CHANNEL_EXTENDED);
    if (module->pool_sv2_channel_type != SV2_CHANNEL_STANDARD) {
        module->pool_sv2_channel_type = SV2_CHANNEL_EXTENDED;
    }
    if (module->fallback_pool_sv2_channel_type != SV2_CHANNEL_STANDARD) {
        module->fallback_pool_sv2_channel_type = SV2_CHANNEL_EXTENDED;
    }
    module->pool_sv2_authority_pubkey = nvs_config_get_string(NVS_CONFIG_STRATUM_SV2_AUTH_KEY, "");
    module->fallback_pool_sv2_authority_pubkey = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_SV2_AUTH_KEY, "");
    module->pool_sv2_require_auth = nvs_config_get_u16(NVS_CONFIG_STRATUM_SV2_REQUIRE_AUTH, 0) != 0;
    module->fallback_pool_sv2_require_auth = nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_SV2_REQUIRE_AUTH, 0) != 0;

    // set fallback to false.
    module->is_using_fallback = false;
    module->pools_unavailable = false;
    module->pool_mode = nvs_config_get_u16(NVS_CONFIG_POOL_MODE, POOL_MODE_FALLBACK);
    if (module->pool_mode > POOL_MODE_DUAL) {
        module->pool_mode = POOL_MODE_FALLBACK;
    }
    module->pool_balance = nvs_config_get_u16(NVS_CONFIG_POOL_BALANCE, 50);
    if (module->pool_balance > 100) {
        module->pool_balance = 50;
    }

    for (int i = 0; i < POOL_COUNT; i++) {
        GLOBAL_STATE->pools[i].transport = NULL;
        GLOBAL_STATE->pools[i].transport_fd = -1;
        pthread_mutex_init(&GLOBAL_STATE->pools[i].transport_lock, NULL);
        GLOBAL_STATE->pools[i].mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
        GLOBAL_STATE->pools[i].send_uid = 1;
        GLOBAL_STATE->pools[i].first_share_uid = 1;
        GLOBAL_STATE->pools[i].connected = false;
        GLOBAL_STATE->pools[i].valid_notify = false;
        GLOBAL_STATE->pools[i].close_requested = false;
        GLOBAL_STATE->pools[i].extranonce_str = NULL;
        GLOBAL_STATE->pools[i].extranonce_2_len = 0;
        GLOBAL_STATE->pools[i].version_mask = 0;
        GLOBAL_STATE->pools[i].new_version_rolling_msg = false;
        GLOBAL_STATE->pools[i].stratum_difficulty = 8192.0;
        GLOBAL_STATE->pools[i].current_notify = NULL;
        GLOBAL_STATE->pools[i].extranonce_2 = 0;
        GLOBAL_STATE->pools[i].shares_accepted = 0;
        GLOBAL_STATE->pools[i].shares_rejected = 0;
        GLOBAL_STATE->pools[i].connect_failures = 0;
        memset(&GLOBAL_STATE->pools[i].coinbase, 0, sizeof(CoinbaseInfo));
    }

    // Initialize overheat_mode
    module->overheat_mode = nvs_config_get_u16(NVS_CONFIG_OVERHEAT_MODE, 0);
    ESP_LOGI(TAG, "Initial overheat_mode value: %d", module->overheat_mode);

    //Initialize power_fault fault mode
    module->power_fault = 0;

    // set the best diff string
    _suffix_string(module->best_nonce_diff, module->best_diff_string, DIFF_STRING_SIZE, 0);
    _suffix_string(module->best_session_nonce_diff, module->best_session_diff_string, DIFF_STRING_SIZE, 0);

    // set the ssid string to blank
    memset(module->ssid, 0, sizeof(module->ssid));

    // set the wifi_status to blank
    memset(module->wifi_status, 0, 20);
}

static void led_timer_callback(TimerHandle_t xTimer)
{
    if (xTimer == led_timer_1) {
        gpio_set_level(BLINK_GPIO_1, 1);
    } else if (xTimer == led_timer_2) {
        gpio_set_level(BLINK_GPIO_2, 1);
    }
}

static esp_err_t configure_led(void)
{
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << BLINK_GPIO_1) | (1ULL << BLINK_GPIO_2);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    gpio_set_level(BLINK_GPIO_1, 1);
    gpio_set_level(BLINK_GPIO_2, 1);

    led_timer_1 = xTimerCreate("LED1", pdMS_TO_TICKS(300), pdFALSE, (void*)0, led_timer_callback);
    led_timer_2 = xTimerCreate("LED2", pdMS_TO_TICKS(300), pdFALSE, (void*)0, led_timer_callback);

    return ESP_OK;
}

esp_err_t SYSTEM_init_peripherals(GlobalState * GLOBAL_STATE) {
    
    ESP_RETURN_ON_ERROR(gpio_install_isr_service(0), TAG, "Error installing ISR service");
    ESP_RETURN_ON_ERROR(configure_led(), TAG, "LED config failed!");

    // Initialize the core voltage regulator
    ESP_RETURN_ON_ERROR(VCORE_init(GLOBAL_STATE), TAG, "VCORE init failed!");
    ESP_RETURN_ON_ERROR(VCORE_set_voltage(nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE, CONFIG_ASIC_VOLTAGE) / 1000.0, GLOBAL_STATE), TAG, "VCORE set voltage failed!");
    ESP_RETURN_ON_ERROR(Thermal_init(GLOBAL_STATE), TAG, "Thermal init failed!");
    ESP_RETURN_ON_ERROR(VCORE_set_voltage(0U, GLOBAL_STATE), TAG, "VCORE set voltage failed!");
    vTaskDelay(500 / portTICK_PERIOD_MS);

    // Ensure overheat_mode config exists
    ESP_RETURN_ON_ERROR(ensure_overheat_mode_config(), TAG, "Failed to ensure overheat_mode config");

    int16_t voltage = VCORE_get_voltage_mv(GLOBAL_STATE);
    ESP_LOGI(TAG, "VCORE: %d", voltage);

    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    return ESP_OK;
}

static void switch_led(int num, uint8_t state)
{
    switch(num)
    {
        case 1:
            gpio_set_level(BLINK_GPIO_1, !state);
            if (state && led_timer_1) {
                xTimerReset(led_timer_1, 0);
            }
            break;
        case 2:
            gpio_set_level(BLINK_GPIO_2, !state);
            if (state && led_timer_2) {
                xTimerReset(led_timer_2, 0);
            }
            break;
        default:
            break;
    }
}

void SYSTEM_notify_accepted_share(GlobalState * GLOBAL_STATE, int pool_id)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    module->shares_accepted++;
    if (pool_id >= 0 && pool_id < POOL_COUNT) {
        GLOBAL_STATE->pools[pool_id].shares_accepted++;
    }
    pthread_mutex_lock(&led_blink_lock);
    if (module->led_blink_enabled) {
        switch_led(1, 1);
    }
    pthread_mutex_unlock(&led_blink_lock);
}

esp_err_t SYSTEM_set_led_blink_enabled(GlobalState * GLOBAL_STATE, bool enabled)
{
    pthread_mutex_lock(&led_blink_lock);
    GLOBAL_STATE->SYSTEM_MODULE.led_blink_enabled = enabled;

    if (!enabled) {
        // Kill any blink already in flight so the LED goes dark immediately.
        // A failed stop is harmless: the one-shot still fires and drives the LED off.
        if (led_timer_1 && xTimerStop(led_timer_1, 0) != pdPASS) {
            ESP_LOGW(TAG, "Could not stop LED timer, blink will end on its own");
        }
        switch_led(1, 0);
    }
    pthread_mutex_unlock(&led_blink_lock);

    // The runtime state is applied above; only persistence can fail from here on.
    esp_err_t err = nvs_config_set_u16(NVS_CONFIG_LED_BLINK, enabled ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Share LED blink %s but not saved: %s", enabled ? "enabled" : "disabled", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Share LED blink %s", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

bool SYSTEM_get_led_blink_enabled(GlobalState * GLOBAL_STATE)
{
    pthread_mutex_lock(&led_blink_lock);
    bool enabled = GLOBAL_STATE->SYSTEM_MODULE.led_blink_enabled;
    pthread_mutex_unlock(&led_blink_lock);
    return enabled;
}

static int compare_rejected_reason_stats(const void *a, const void *b) {
    const RejectedReasonStat *ea = a;
    const RejectedReasonStat *eb = b;
    return (eb->count > ea->count) - (ea->count > eb->count);
}

void SYSTEM_notify_rejected_share(GlobalState * GLOBAL_STATE, int pool_id, char * error_msg)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    module->shares_rejected++;
    if (pool_id >= 0 && pool_id < POOL_COUNT) {
        GLOBAL_STATE->pools[pool_id].shares_rejected++;
    }

    for (int i = 0; i < module->rejected_reason_stats_count; i++) {
        if (strncmp(module->rejected_reason_stats[i].message, error_msg, sizeof(module->rejected_reason_stats[i].message) - 1) == 0) {
            module->rejected_reason_stats[i].count++;
            return;
        }
    }

    if (module->rejected_reason_stats_count < (int)(sizeof(module->rejected_reason_stats) / sizeof(module->rejected_reason_stats[0]))) {
        strncpy(module->rejected_reason_stats[module->rejected_reason_stats_count].message, 
                error_msg, 
                sizeof(module->rejected_reason_stats[module->rejected_reason_stats_count].message) - 1);
        module->rejected_reason_stats[module->rejected_reason_stats_count].message[sizeof(module->rejected_reason_stats[module->rejected_reason_stats_count].message) - 1] = '\0'; // Ensure null termination
        module->rejected_reason_stats[module->rejected_reason_stats_count].count = 1;
        module->rejected_reason_stats_count++;
    }

    if (module->rejected_reason_stats_count > 1) {
        qsort(module->rejected_reason_stats, module->rejected_reason_stats_count, 
            sizeof(module->rejected_reason_stats[0]), compare_rejected_reason_stats);
    }    
}

void SYSTEM_notify_new_ntime(GlobalState * GLOBAL_STATE, uint32_t ntime)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    // Hourly clock sync
    if (module->lastClockSync + (60 * 60) > ntime) {
        return;
    }
    ESP_LOGI(TAG, "Syncing clock");
    module->lastClockSync = ntime;
    struct timeval tv;
    tv.tv_sec = ntime;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
}

// Hashes are kept in raw digest order and stored in NVS in display order
// (byte-reversed hex), which is what block explorers show.
static void _hash_to_display_hex(const uint8_t hash[32], char out[65])
{
    uint8_t tmp[32];
    memcpy(tmp, hash, 32);
    reverse_bytes(tmp, 32);
    bin2hex(tmp, 32, out, 65);
}

static bool _load_hash(const char * key, uint8_t out[32])
{
    memset(out, 0, 32);
    char * hex = nvs_config_get_string(key, "");
    bool ok = hex != NULL && strlen(hex) == 64 && hex2bin(hex, out, 32) == 32;
    free(hex);
    if (!ok) {
        memset(out, 0, 32);
        return false;
    }
    reverse_bytes(out, 32);
    return true;
}

static void _load_block_state(SystemModule * module)
{
    module->blocks_found = nvs_config_get_u64(NVS_CONFIG_BLOCKS_FOUND, 0);
    module->last_block_time = (uint32_t) nvs_config_get_u64(NVS_CONFIG_BLOCK_TIME, 0);
    module->last_block_status = nvs_config_get_u16(NVS_CONFIG_BLOCK_STATUS, BLOCK_STATUS_NONE);
    bool have_hash = _load_hash(NVS_CONFIG_BLOCK_HASH, module->last_block_hash);
    bool have_parent = _load_hash(NVS_CONFIG_BLOCK_PARENT, module->last_block_parent);

    if (module->last_block_status > BLOCK_STATUS_ORPHANED ||
        (module->last_block_status != BLOCK_STATUS_NONE && !have_hash) ||
        (module->last_block_status == BLOCK_STATUS_PENDING && !have_parent)) {
        ESP_LOGW(TAG, "Inconsistent stored block state, clearing status");
        module->last_block_status = BLOCK_STATUS_NONE;
    }
}

static void _set_block_status(SystemModule * module, block_status_t status)
{
    module->last_block_status = status;
    nvs_config_set_u16(NVS_CONFIG_BLOCK_STATUS, status);
}

static void _check_for_block(GlobalState * GLOBAL_STATE, uint8_t job_id, const uint8_t hash[32])
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;
    const bm_job * job = GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id];
    if (job == NULL) {
        return;
    }

    // Exact consensus check: header hash <= target from the header's nBits
    uint8_t target[32];
    if (!nbits_to_target(job->target, target) || !hash_meets_target(hash, target)) {
        return;
    }

    pthread_mutex_lock(&block_lock);

    // The ASIC can report the same nonce twice; count each block once
    if (module->last_block_status != BLOCK_STATUS_NONE && memcmp(module->last_block_hash, hash, 32) == 0) {
        pthread_mutex_unlock(&block_lock);
        return;
    }

    memcpy(module->last_block_hash, hash, 32);
    memcpy(module->last_block_parent, job->prev_block_hash, 32);
    module->last_block_time = job->ntime;
    module->blocks_found++;

    char hash_hex[65];
    char parent_hex[65];
    _hash_to_display_hex(module->last_block_hash, hash_hex);
    _hash_to_display_hex(module->last_block_parent, parent_hex);

    nvs_config_set_string(NVS_CONFIG_BLOCK_HASH, hash_hex);
    nvs_config_set_string(NVS_CONFIG_BLOCK_PARENT, parent_hex);
    nvs_config_set_u64(NVS_CONFIG_BLOCK_TIME, module->last_block_time);
    nvs_config_set_u64(NVS_CONFIG_BLOCKS_FOUND, module->blocks_found);
    _set_block_status(module, BLOCK_STATUS_PENDING);

    pthread_mutex_unlock(&block_lock);

    ESP_LOGI(TAG, "FOUND BLOCK!!! hash %s (block #%" PRIu64 " found by this device)", hash_hex, module->blocks_found);
}

void SYSTEM_notify_found_nonce(GlobalState * GLOBAL_STATE, double found_diff, uint8_t job_id, const uint8_t hash[32])
{
    // Checked before the best-diff bookkeeping, which returns early when the
    // share doesn't beat the stored all-time best
    _check_for_block(GLOBAL_STATE, job_id, hash);
    _check_for_best_diff(GLOBAL_STATE, found_diff, job_id);
}

void SYSTEM_notify_new_prev_hash(GlobalState * GLOBAL_STATE, const uint8_t prev_hash[32])
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    pthread_mutex_lock(&block_lock);

    if (module->last_block_status != BLOCK_STATUS_PENDING) {
        pthread_mutex_unlock(&block_lock);
        return;
    }

    if (memcmp(prev_hash, module->last_block_hash, 32) == 0) {
        // Pool is now mining on top of our block
        _set_block_status(module, BLOCK_STATUS_CONFIRMED);
        pthread_mutex_unlock(&block_lock);
        ESP_LOGI(TAG, "Block confirmed: pool is building on our block");
        return;
    }

    if (memcmp(prev_hash, module->last_block_parent, 32) != 0) {
        // The tip moved to something that is neither our block nor its parent
        _set_block_status(module, BLOCK_STATUS_ORPHANED);
        pthread_mutex_unlock(&block_lock);
        ESP_LOGW(TAG, "Block not built on: chain moved to a different block");
        return;
    }

    pthread_mutex_unlock(&block_lock);
}

void SYSTEM_get_block_info(GlobalState * GLOBAL_STATE, system_block_info_t * out)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    pthread_mutex_lock(&block_lock);
    out->count = module->blocks_found;
    out->status = (block_status_t) module->last_block_status;
    out->time = module->last_block_time;
    if (module->last_block_status != BLOCK_STATUS_NONE) {
        _hash_to_display_hex(module->last_block_hash, out->hash);
    } else {
        out->hash[0] = '\0';
    }
    pthread_mutex_unlock(&block_lock);
}

void SYSTEM_clear_block_info(GlobalState * GLOBAL_STATE)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    pthread_mutex_lock(&block_lock);
    module->blocks_found = 0;
    module->last_block_time = 0;
    memset(module->last_block_hash, 0, 32);
    memset(module->last_block_parent, 0, 32);

    nvs_config_set_u64(NVS_CONFIG_BLOCKS_FOUND, 0);
    nvs_config_set_u64(NVS_CONFIG_BLOCK_TIME, 0);
    nvs_config_set_string(NVS_CONFIG_BLOCK_HASH, "");
    nvs_config_set_string(NVS_CONFIG_BLOCK_PARENT, "");
    _set_block_status(module, BLOCK_STATUS_NONE);
    pthread_mutex_unlock(&block_lock);

    ESP_LOGI(TAG, "Found-block data cleared");
}

const char * SYSTEM_block_status_string(block_status_t status)
{
    switch (status) {
    case BLOCK_STATUS_PENDING:
        return "pending";
    case BLOCK_STATUS_CONFIRMED:
        return "confirmed";
    case BLOCK_STATUS_ORPHANED:
        return "orphaned";
    default:
        return "none";
    }
}

static double _calculate_network_difficulty(uint32_t nBits)
{
    uint32_t mantissa = nBits & 0x007fffff;  // Extract the mantissa from nBits
    uint8_t exponent = (nBits >> 24) & 0xff; // Extract the exponent from nBits

    double target = (double) mantissa * pow(256, (exponent - 3)); // Calculate the target value

    double difficulty = (pow(2, 208) * 65535) / target; // Calculate the difficulty

    return difficulty;
}

static void _check_for_best_diff(GlobalState * GLOBAL_STATE, double diff, uint8_t job_id)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    if ((uint64_t) diff > module->best_session_nonce_diff) {
        module->best_session_nonce_diff = (uint64_t) diff;
        _suffix_string((uint64_t) diff, module->best_session_diff_string, DIFF_STRING_SIZE, 0);
    }

    if ((uint64_t) diff <= module->best_nonce_diff) {
        return;
    }
    module->best_nonce_diff = (uint64_t) diff;

    nvs_config_set_u64(NVS_CONFIG_BEST_DIFF, module->best_nonce_diff);

    // make the best_nonce_diff into a string
    _suffix_string((uint64_t) diff, module->best_diff_string, DIFF_STRING_SIZE, 0);

    double network_diff = _calculate_network_difficulty(GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]->target);
    ESP_LOGI(TAG, "New best diff %s, network diff: %f", module->best_diff_string, network_diff);
}

/* Convert a uint64_t value into a truncated string for displaying with its
 * associated suitable for Mega, Giga etc. Buf array needs to be long enough */
static void _suffix_string(uint64_t val, char * buf, size_t bufsiz, int sigdigits)
{
    const double dkilo = 1000.0;
    const uint64_t kilo = 1000ull;
    const uint64_t mega = 1000000ull;
    const uint64_t giga = 1000000000ull;
    const uint64_t tera = 1000000000000ull;
    const uint64_t peta = 1000000000000000ull;
    const uint64_t exa = 1000000000000000000ull;
    char suffix[2] = "";
    bool decimal = true;
    double dval;

    if (val >= exa) {
        val /= peta;
        dval = (double) val / dkilo;
        strcpy(suffix, "E");
    } else if (val >= peta) {
        val /= tera;
        dval = (double) val / dkilo;
        strcpy(suffix, "P");
    } else if (val >= tera) {
        val /= giga;
        dval = (double) val / dkilo;
        strcpy(suffix, "T");
    } else if (val >= giga) {
        val /= mega;
        dval = (double) val / dkilo;
        strcpy(suffix, "G");
    } else if (val >= mega) {
        val /= kilo;
        dval = (double) val / dkilo;
        strcpy(suffix, "M");
    } else if (val >= kilo) {
        dval = (double) val / dkilo;
        strcpy(suffix, "k");
    } else {
        dval = val;
        decimal = false;
    }

    if (!sigdigits) {
        if (decimal)
            snprintf(buf, bufsiz, "%.3g%s", dval, suffix);
        else
            snprintf(buf, bufsiz, "%d%s", (unsigned int) dval, suffix);
    } else {
        /* Always show sigdigits + 1, padded on right with zeroes
         * followed by suffix */
        int ndigits = sigdigits - 1 - (dval > 0.0 ? floor(log10(dval)) : 0);

        snprintf(buf, bufsiz, "%*.*f%s", sigdigits + 1, ndigits, dval, suffix);
    }
}

static esp_err_t ensure_overheat_mode_config() {
    uint16_t overheat_mode = nvs_config_get_u16(NVS_CONFIG_OVERHEAT_MODE, UINT16_MAX);

    if (overheat_mode == UINT16_MAX) {
        // Key doesn't exist or couldn't be read, set the default value
        nvs_config_set_u16(NVS_CONFIG_OVERHEAT_MODE, 0);
        ESP_LOGI(TAG, "Default value for overheat_mode set to 0");
    } else {
        // Key exists, log the current value
        ESP_LOGI(TAG, "Existing overheat_mode value: %d", overheat_mode);
    }

    return ESP_OK;
}
