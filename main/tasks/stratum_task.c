#include "esp_log.h"
#include "connect.h"
#include "system.h"
#include "global_state.h"
#include "lwip/dns.h"
#include <lwip/tcpip.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include "nvs_config.h"
#include "stratum_task.h"
#include "work_queue.h"
#include "esp_wifi.h"
#include <esp_sntp.h>
#include <time.h>
#include <string.h>

#define PORT CONFIG_STRATUM_PORT
#define STRATUM_URL CONFIG_STRATUM_URL
#define STRATUM_TLS CONFIG_STRATUM_TLS
#define STRATUM_CERT CONFIG_STRATUM_CERT

#define FALLBACK_PORT CONFIG_FALLBACK_STRATUM_PORT
#define FALLBACK_STRATUM_URL CONFIG_FALLBACK_STRATUM_URL
#define FALLBACK_STRATUM_TLS CONFIG_FALLBACK_STRATUM_TLS
#define FALLBACK_STRATUM_CERT CONFIG_FALLBACK_STRATUM_CERT

#define STRATUM_PW CONFIG_STRATUM_PW
#define FALLBACK_STRATUM_PW CONFIG_FALLBACK_STRATUM_PW
#define STRATUM_DIFFICULTY CONFIG_STRATUM_DIFFICULTY

#define MAX_RETRY_ATTEMPTS 3
#define MAX_CRITICAL_RETRY_ATTEMPTS 5

#define TRANSPORT_TIMEOUT_MS 5000

#define BUFFER_SIZE 1024

static const char * TAG = "stratum_task";

static StratumApiV1Message stratum_api_v1_message = {};
static SystemTaskModule SYSTEM_TASK_MODULE = {.stratum_difficulty = 8192};

static const char * primary_stratum_url;
static uint16_t primary_stratum_port;

struct timeval tcp_snd_timeout = {
    .tv_sec = 5,
    .tv_usec = 0
};

struct timeval tcp_rcv_timeout = {
    .tv_sec = 60 * 3,
    .tv_usec = 0
};

static uint16_t primary_stratum_tls;
static char * primary_stratum_cert;

static void set_socket_options(esp_transport_handle_t transport)
{
    int sock = esp_transport_get_socket(transport);
    if (sock >= 0) {
        if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tcp_snd_timeout, sizeof(tcp_snd_timeout)) < 0) {
            ESP_LOGE(TAG, "Failed to set SO_SNDTIMEO");
        }
        if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tcp_rcv_timeout, sizeof(tcp_rcv_timeout)) < 0) {
            ESP_LOGE(TAG, "Failed to set SO_RCVTIMEO");
        }

        int keepalive = 1;
        if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) < 0) {
            ESP_LOGE(TAG, "Failed to set SO_KEEPALIVE");
        }

        int keepidle = 60;
        int keepintvl = 10;
        int keepcnt = 3;
        if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle)) < 0) {
            ESP_LOGE(TAG, "Failed to set TCP_KEEPIDLE");
        }
        if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl)) < 0) {
            ESP_LOGE(TAG, "Failed to set TCP_KEEPINTVL");
        }
        if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt)) < 0) {
            ESP_LOGE(TAG, "Failed to set TCP_KEEPCNT");
        }
    } else {
        ESP_LOGE(TAG, "Failed to get socket from transport");
    }
}

bool is_wifi_connected() {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return true;
    } else {
        return false;
    }
}

void cleanQueue(GlobalState * GLOBAL_STATE) {
    ESP_LOGI(TAG, "Clean Jobs: clearing queue");
    GLOBAL_STATE->abandon_work = 1;
    queue_clear(&GLOBAL_STATE->stratum_queue);

    pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock);
    ASIC_jobs_queue_clear(&GLOBAL_STATE->ASIC_jobs_queue);
    for (int i = 0; i < 128; i = i + 4) {
        GLOBAL_STATE->valid_jobs[i] = 0;
    }
    pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);
}

void stratum_reset_uid(GlobalState * GLOBAL_STATE)
{
    ESP_LOGI(TAG, "Resetting stratum uid");
    GLOBAL_STATE->send_uid = 1;
}

void stratum_close_connection(GlobalState * GLOBAL_STATE)
{
    ESP_LOGE(TAG, "Shutting down socket and restarting...");
    if (GLOBAL_STATE->transport != NULL) {
        esp_transport_close(GLOBAL_STATE->transport);
        GLOBAL_STATE->transport = NULL;
    }
    cleanQueue(GLOBAL_STATE);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
}

void stratum_primary_heartbeat(void * pvParameters)
{
    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    ESP_LOGI(TAG, "Starting heartbeat thread for primary pool: %s:%d", primary_stratum_url, primary_stratum_port);
    vTaskDelay(10000 / portTICK_PERIOD_MS);

    while (1)
    {
        if (GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback == false) {
            vTaskDelay(10000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGD(TAG, "Running Heartbeat on: %s!", primary_stratum_url);

        if (!is_wifi_connected()) {
            ESP_LOGD(TAG, "Heartbeat. Failed WiFi check!");
            vTaskDelay(10000 / portTICK_PERIOD_MS);
            continue;
        }

        struct hostent *primary_dns_addr = gethostbyname(primary_stratum_url);
        if (primary_dns_addr == NULL) {
            ESP_LOGD(TAG, "Heartbeat. Failed DNS check for: %s!", primary_stratum_url);
            vTaskDelay(60000 / portTICK_PERIOD_MS);
            continue;
        }

        tls_mode tls = primary_stratum_tls;
        char * cert = primary_stratum_cert;
        esp_transport_handle_t transport = STRATUM_V1_transport_init(tls, cert);
        if (transport == NULL) {
            ESP_LOGD(TAG, "Heartbeat. Failed transport init check!");
            vTaskDelay(60000 / portTICK_PERIOD_MS);
            continue;
        }

        esp_err_t err = esp_transport_connect(transport, primary_stratum_url, primary_stratum_port, TRANSPORT_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "Heartbeat. Failed connect check: %s:%d (errno %d: %s)", primary_stratum_url, primary_stratum_port, err, strerror(err));
            esp_transport_close(transport);
            vTaskDelay(60000 / portTICK_PERIOD_MS);
            continue;
        }

        set_socket_options(transport);

        int send_uid = 1;
        STRATUM_V1_subscribe(transport, send_uid++, GLOBAL_STATE->asic_model_str);
        STRATUM_V1_authorize(transport, send_uid++, GLOBAL_STATE->SYSTEM_MODULE.pool_user, GLOBAL_STATE->SYSTEM_MODULE.pool_pass);

        char recv_buffer[BUFFER_SIZE];
        memset(recv_buffer, 0, BUFFER_SIZE);
        int bytes_received = esp_transport_read(transport, recv_buffer, BUFFER_SIZE - 1, TRANSPORT_TIMEOUT_MS);

        esp_transport_close(transport);

        if (bytes_received == -1)  {
            vTaskDelay(60000 / portTICK_PERIOD_MS);
            continue;
        }

        if (strstr(recv_buffer, "mining.notify") != NULL) {
            ESP_LOGI(TAG, "Heartbeat successful and in fallback mode. Switching back to primary.");
            GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback = false;
            stratum_close_connection(GLOBAL_STATE);
            continue;
        }

        vTaskDelay(60000 / portTICK_PERIOD_MS);
    }
}

void stratum_task(void * pvParameters)
{
    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    primary_stratum_url = GLOBAL_STATE->SYSTEM_MODULE.pool_url;
    primary_stratum_port = GLOBAL_STATE->SYSTEM_MODULE.pool_port;
    primary_stratum_tls = GLOBAL_STATE->SYSTEM_MODULE.pool_tls;
    primary_stratum_cert = GLOBAL_STATE->SYSTEM_MODULE.pool_cert;
    char * stratum_url = GLOBAL_STATE->SYSTEM_MODULE.pool_url;
    uint16_t port = GLOBAL_STATE->SYSTEM_MODULE.pool_port;
    tls_mode tls = GLOBAL_STATE->SYSTEM_MODULE.pool_tls;
    char * cert = GLOBAL_STATE->SYSTEM_MODULE.pool_cert;

    STRATUM_V1_initialize_buffer();
    int retry_attempts = 0;
    int retry_critical_attempts = 0;

    xTaskCreate(stratum_primary_heartbeat, "stratum primary heartbeat", 8192, pvParameters, 1, NULL);

    ESP_LOGI(TAG, "Opening connection to pool: %s:%d", stratum_url, port);
    while (1) {
        if (!is_wifi_connected()) {
            ESP_LOGI(TAG, "WiFi disconnected, attempting to reconnect...");
            vTaskDelay(10000 / portTICK_PERIOD_MS);
            continue;
        }

        if (retry_attempts >= MAX_RETRY_ATTEMPTS)
        {
            if (GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_url == NULL || GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_url[0] == '\0') {
                ESP_LOGI(TAG, "Unable to switch to fallback. No url configured. (retries: %d)...", retry_attempts);
                GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback = false;
                retry_attempts = 0;
                continue;
            }

            GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback = !GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback;
            ESP_LOGI(TAG, "Switching target due to too many failures (retries: %d)...", retry_attempts);
            retry_attempts = 0;
        }

        stratum_url = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_url : GLOBAL_STATE->SYSTEM_MODULE.pool_url;
        port = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_port : GLOBAL_STATE->SYSTEM_MODULE.pool_port;

        struct hostent *dns_addr = gethostbyname(stratum_url);
        if (dns_addr == NULL) {
            ESP_LOGE(TAG, "DNS resolution failed for %s", stratum_url);
            retry_attempts++;
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        char host_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, (void *)dns_addr->h_addr_list[0], host_ip, sizeof(host_ip));

        ESP_LOGI(TAG, "Connecting to: stratum+tcp://%s:%d (%s)", stratum_url, port, host_ip);

        tls = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_tls : GLOBAL_STATE->SYSTEM_MODULE.pool_tls;
        cert = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_cert : GLOBAL_STATE->SYSTEM_MODULE.pool_cert;

        GLOBAL_STATE->transport = STRATUM_V1_transport_init(tls, cert);
        if (GLOBAL_STATE->transport == NULL) {
            ESP_LOGE(TAG, "Transport initialization failed.");
            if (++retry_critical_attempts > MAX_CRITICAL_RETRY_ATTEMPTS) {
                ESP_LOGE(TAG, "Max retry attempts reached, restarting...");
                esp_restart();
            }
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            continue;
        }
        retry_critical_attempts = 0;

        ESP_LOGI(TAG, "Transport initialized, connecting to %s:%d", stratum_url, port);
        esp_err_t ret = esp_transport_connect(GLOBAL_STATE->transport, stratum_url, port, TRANSPORT_TIMEOUT_MS);
        if (ret != ESP_OK) {
            retry_attempts++;
            ESP_LOGE(TAG, "Transport unable to connect to %s:%d (errno %d). Attempt: %d", stratum_url, port, ret, retry_attempts);
            esp_transport_close(GLOBAL_STATE->transport);
            GLOBAL_STATE->transport = NULL;
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            continue;
        }

        set_socket_options(GLOBAL_STATE->transport);

        const char *tls_status;
        switch (tls) {
            case DISABLED:     tls_status = ""; break;
            case BUNDLED_CRT:  tls_status = " (TLS)"; break;
            case CUSTOM_CRT:   tls_status = " (TLS Cert)"; break;
            default:           tls_status = ""; break;
        }
        ESP_LOGI(TAG, "Connected to %s:%d%s", stratum_url, port, tls_status);

        stratum_reset_uid(GLOBAL_STATE);
        cleanQueue(GLOBAL_STATE);

        ///// Start Stratum Action
        // mining.configure - ID: 1
        STRATUM_V1_configure_version_rolling(GLOBAL_STATE->transport, GLOBAL_STATE->send_uid++, &GLOBAL_STATE->version_mask);

        // mining.subscribe - ID: 2
        STRATUM_V1_subscribe(GLOBAL_STATE->transport, GLOBAL_STATE->send_uid++, GLOBAL_STATE->asic_model_str);

        char * username = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_user : GLOBAL_STATE->SYSTEM_MODULE.pool_user;
        char * password = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_pass : GLOBAL_STATE->SYSTEM_MODULE.pool_pass;

        //mining.authorize - ID: 3
        STRATUM_V1_authorize(GLOBAL_STATE->transport, GLOBAL_STATE->send_uid++, username, password);

        //mining.suggest_difficulty - ID: 4
        STRATUM_V1_suggest_difficulty(GLOBAL_STATE->transport, GLOBAL_STATE->send_uid++, STRATUM_DIFFICULTY);

        // Everything is set up, lets make sure we don't abandon work unnecessarily.
        GLOBAL_STATE->abandon_work = 0;

        while (1) {
            char * line = STRATUM_V1_receive_jsonrpc_line(GLOBAL_STATE->transport);
            if (!line) {
                ESP_LOGE(TAG, "Failed to receive JSON-RPC line, reconnecting...");
                retry_attempts++;
                stratum_close_connection(GLOBAL_STATE);
                break;
            }

            STRATUM_V1_parse(&stratum_api_v1_message, line);
            free(line);

            if (stratum_api_v1_message.method == MINING_NOTIFY) {
                SYSTEM_notify_new_ntime(GLOBAL_STATE, stratum_api_v1_message.mining_notification->ntime);
                if (stratum_api_v1_message.mining_notification->clean_jobs &&
                    (GLOBAL_STATE->stratum_queue.count > 0 || GLOBAL_STATE->ASIC_jobs_queue.count > 0)) {
                    cleanQueue(GLOBAL_STATE);
                }
                if (GLOBAL_STATE->stratum_queue.count == QUEUE_SIZE) {
                    mining_notify * next_notify_json_str = (mining_notify *) queue_dequeue(&GLOBAL_STATE->stratum_queue);
                    STRATUM_V1_free_mining_notify(next_notify_json_str);
                }
                stratum_api_v1_message.mining_notification->difficulty = SYSTEM_TASK_MODULE.stratum_difficulty;
                queue_enqueue(&GLOBAL_STATE->stratum_queue, stratum_api_v1_message.mining_notification);
            } else if (stratum_api_v1_message.method == MINING_SET_DIFFICULTY) {
                if (stratum_api_v1_message.new_difficulty != SYSTEM_TASK_MODULE.stratum_difficulty) {
                    SYSTEM_TASK_MODULE.stratum_difficulty = stratum_api_v1_message.new_difficulty;
                    ESP_LOGI(TAG, "Set stratum difficulty: %ld", SYSTEM_TASK_MODULE.stratum_difficulty);
                }
            } else if (stratum_api_v1_message.method == MINING_SET_VERSION_MASK ||
                    stratum_api_v1_message.method == STRATUM_RESULT_VERSION_MASK) {
                ESP_LOGI(TAG, "Set version mask: %08lx", stratum_api_v1_message.version_mask);
                GLOBAL_STATE->version_mask = stratum_api_v1_message.version_mask;
                GLOBAL_STATE->new_stratum_version_rolling_msg = true;
            } else if (stratum_api_v1_message.method == MINING_SET_EXTRANONCE ||
                    stratum_api_v1_message.method == STRATUM_RESULT_SUBSCRIBE) {
                char *old_extranonce = GLOBAL_STATE->extranonce_str;
                GLOBAL_STATE->extranonce_str = stratum_api_v1_message.extranonce_str;
                GLOBAL_STATE->extranonce_2_len = stratum_api_v1_message.extranonce_2_len;
                free(old_extranonce);
            } else if (stratum_api_v1_message.method == MINING_PING) {
                STRATUM_V1_pong(GLOBAL_STATE->transport, stratum_api_v1_message.message_id);
            } else if (stratum_api_v1_message.method == CLIENT_RECONNECT) {
                ESP_LOGE(TAG, "Pool requested client reconnect...");
                stratum_close_connection(GLOBAL_STATE);
                break;
            } else if (stratum_api_v1_message.method == STRATUM_RESULT) {
                if (stratum_api_v1_message.response_success) {
                    ESP_LOGI(TAG, "message result accepted");
                    SYSTEM_notify_accepted_share(GLOBAL_STATE);
                } else {
                    ESP_LOGW(TAG, "message result rejected: %s", stratum_api_v1_message.error_str);
                    SYSTEM_notify_rejected_share(GLOBAL_STATE, stratum_api_v1_message.error_str);
                }
            } else if (stratum_api_v1_message.method == STRATUM_RESULT_SETUP) {
                // Reset retry attempts after successfully receiving data.
                retry_attempts = 0;
                if (stratum_api_v1_message.response_success) {
                    ESP_LOGI(TAG, "setup message accepted");
                } else {
                    ESP_LOGE(TAG, "setup message rejected: %s", stratum_api_v1_message.error_str);
                }
            }
        }

        if (stratum_api_v1_message.error_str) {
            free(stratum_api_v1_message.error_str);
            stratum_api_v1_message.error_str = NULL;
        }
    }
    vTaskDelete(NULL);
}
