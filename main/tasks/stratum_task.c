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
#include "esp_netif.h"
#include "esp_transport_ssl.h"
#include <esp_sntp.h>
#include <time.h>
#include <string.h>
#include <inttypes.h>
#include "coinbase_decoder.h"

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

static SystemTaskModule SYSTEM_TASK_MODULE = {.stratum_difficulty = 8192.0};

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

typedef struct {
    GlobalState *state;
    int pool_id;
} stratum_pool_task_params_t;

typedef struct {
    struct sockaddr_storage dest_addr;
    socklen_t addrlen;
    int addr_family;
    int ip_protocol;
    char host_ip[INET6_ADDRSTRLEN + 16];
} stratum_connection_info_t;

static esp_err_t resolve_stratum_address(const char *hostname, uint16_t port, stratum_connection_info_t *conn_info)
{
    if (hostname == NULL || conn_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (port == 0) {
        ESP_LOGE(TAG, "Invalid port: 0");
        return ESP_ERR_INVALID_ARG;
    }

    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = IPPROTO_TCP,
        .ai_flags = AI_NUMERICSERV,
    };

    struct addrinfo *res = NULL;
    int gai_err = getaddrinfo(hostname, port_str, &hints, &res);
    if (gai_err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS resolution failed for %s:%u (error: %d)", hostname, port, gai_err);
        return ESP_ERR_NOT_FOUND;
    }

    memset(conn_info, 0, sizeof(*conn_info));
    conn_info->addr_family = AF_UNSPEC;

    const int preferred_families[] = { AF_INET, AF_INET6 };
    const struct addrinfo *selected = NULL;

    for (size_t i = 0; i < sizeof(preferred_families) / sizeof(preferred_families[0]) && selected == NULL; i++) {
        for (const struct addrinfo *p = res; p != NULL; p = p->ai_next) {
            if (p->ai_family == preferred_families[i]) {
                selected = p;
                break;
            }
        }
    }

    if (selected == NULL) {
        ESP_LOGE(TAG, "No supported address family found for %s", hostname);
        freeaddrinfo(res);
        return ESP_ERR_NOT_SUPPORTED;
    }

    memcpy(&conn_info->dest_addr, selected->ai_addr, selected->ai_addrlen);
    conn_info->addrlen = selected->ai_addrlen;
    conn_info->addr_family = selected->ai_family;
    conn_info->ip_protocol = (selected->ai_family == AF_INET) ? IPPROTO_IP : IPPROTO_IPV6;

    if (selected->ai_family == AF_INET6) {
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&conn_info->dest_addr;
        if (IN6_IS_ADDR_LINKLOCAL(&addr6->sin6_addr) && addr6->sin6_scope_id == 0) {
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif) {
                int index = esp_netif_get_netif_impl_index(netif);
                if (index >= 0) {
                    addr6->sin6_scope_id = (uint32_t)index;
                }
            }
        }
    }

    const void *src_addr = NULL;
    if (conn_info->addr_family == AF_INET) {
        struct sockaddr_in *addr4 = (struct sockaddr_in *)&conn_info->dest_addr;
        src_addr = &addr4->sin_addr;
    } else {
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&conn_info->dest_addr;
        src_addr = &addr6->sin6_addr;
    }

    if (inet_ntop(conn_info->addr_family, src_addr, conn_info->host_ip, sizeof(conn_info->host_ip)) == NULL) {
        ESP_LOGW(TAG, "inet_ntop failed (errno: %d)", errno);
        snprintf(conn_info->host_ip, sizeof(conn_info->host_ip), "[invalid %s addr]",
                 (conn_info->addr_family == AF_INET) ? "IPv4" : "IPv6");
    } else if (conn_info->addr_family == AF_INET6) {
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&conn_info->dest_addr;
        if (IN6_IS_ADDR_LINKLOCAL(&addr6->sin6_addr) && addr6->sin6_scope_id != 0) {
            char zone[16];
            snprintf(zone, sizeof(zone), "%%%lu", (unsigned long)addr6->sin6_scope_id);
            strncat(conn_info->host_ip, zone, sizeof(conn_info->host_ip) - strlen(conn_info->host_ip) - 1);
            conn_info->host_ip[sizeof(conn_info->host_ip) - 1] = '\0';
        }
    }

    ESP_LOGI(TAG, "Resolved %s:%u -> %s", hostname, port, conn_info->host_ip);

    freeaddrinfo(res);
    return ESP_OK;
}

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

// ---------------------------------------------------------------------------
// transport ownership
//
// A transport may only be closed and destroyed by the worker task that created
// it. Every other task -- the primary heartbeat, ASIC_result_task -- can only
// *request* a close. This is what keeps a worker that is parked inside
// esp_transport_read() (up to SO_RCVTIMEO, three minutes) from having the
// handle freed underneath it.
//
// `lock` guards the handle and its socket fd together. Rules:
//   - the worker publishes the pair with conn_publish() after connecting,
//   - anyone using the transport (e.g. writing a share) holds `lock` for the
//     whole operation, so the worker cannot destroy it mid-write,
//   - the worker takes the pair away with conn_claim() before destroying, so
//     once a user sees NULL the handle is gone and must not be touched,
//   - conn_request_close() kicks the worker out of its blocking read with
//     shutdown(), which leaves the fd allocated -- no window in which the
//     worker could read from a recycled descriptor -- and never frees anything.
// ---------------------------------------------------------------------------

static void conn_publish(pthread_mutex_t *lock, esp_transport_handle_t *slot, int *fd_slot,
                         esp_transport_handle_t transport)
{
    pthread_mutex_lock(lock);
    *slot = transport;
    *fd_slot = esp_transport_get_socket(transport);
    pthread_mutex_unlock(lock);
}

// Takes the handle out of the shared slot so the caller can destroy it.
// Returns NULL when there is nothing to destroy.
static esp_transport_handle_t conn_claim(pthread_mutex_t *lock, esp_transport_handle_t *slot, int *fd_slot)
{
    pthread_mutex_lock(lock);
    esp_transport_handle_t transport = *slot;
    *slot = NULL;
    *fd_slot = -1;
    pthread_mutex_unlock(lock);
    return transport;
}

// Wakes a worker blocked in esp_transport_read() without altering the
// transport's lifetime. Held across the syscall so the owning worker cannot
// close the descriptor while we are naming it.
static void conn_request_close(pthread_mutex_t *lock, int *fd_slot)
{
    pthread_mutex_lock(lock);
    if (*fd_slot >= 0) {
        shutdown(*fd_slot, SHUT_RDWR);
    }
    pthread_mutex_unlock(lock);
}

static const char *pool_label(int pool_id)
{
    return pool_id == POOL_SECONDARY ? "secondary" : "primary";
}

static char *pool_url(GlobalState *GLOBAL_STATE, int pool_id)
{
    return pool_id == POOL_SECONDARY ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_url : GLOBAL_STATE->SYSTEM_MODULE.pool_url;
}

static uint16_t pool_port(GlobalState *GLOBAL_STATE, int pool_id)
{
    return pool_id == POOL_SECONDARY ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_port : GLOBAL_STATE->SYSTEM_MODULE.pool_port;
}

static tls_mode pool_tls(GlobalState *GLOBAL_STATE, int pool_id)
{
    return pool_id == POOL_SECONDARY ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_tls : GLOBAL_STATE->SYSTEM_MODULE.pool_tls;
}

static char *pool_cert(GlobalState *GLOBAL_STATE, int pool_id)
{
    return pool_id == POOL_SECONDARY ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_cert : GLOBAL_STATE->SYSTEM_MODULE.pool_cert;
}

static char *pool_user(GlobalState *GLOBAL_STATE, int pool_id)
{
    return pool_id == POOL_SECONDARY ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_user : GLOBAL_STATE->SYSTEM_MODULE.pool_user;
}

static char *pool_pass(GlobalState *GLOBAL_STATE, int pool_id)
{
    return pool_id == POOL_SECONDARY ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_pass : GLOBAL_STATE->SYSTEM_MODULE.pool_pass;
}

// Decode the coinbase transaction of a mining.notify so the web UI can show
// where the block reward would go. Purely informational: a notification we
// cannot parse is dropped without touching the mining path.
static void decode_mining_notification(GlobalState *GLOBAL_STATE, int pool_id,
                                       const mining_notify *notification,
                                       const char *extranonce_str, int extranonce_2_len)
{
    if (notification == NULL || extranonce_str == NULL) {
        return;
    }

    mining_notification_result_t *result = calloc(1, sizeof(mining_notification_result_t));
    if (result == NULL) {
        ESP_LOGW(TAG, "Failed to allocate coinbase decode result");
        return;
    }

    esp_err_t err = coinbase_process_notification(notification, extranonce_str, extranonce_2_len,
                                                  pool_user(GLOBAL_STATE, pool_id), result);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Could not decode %s coinbase transaction: %s", pool_label(pool_id), esp_err_to_name(err));
        free(result->scriptsig);
        free(result);
        return;
    }

    CoinbaseInfo *coinbase = &GLOBAL_STATE->pools[pool_id].coinbase;

    pthread_mutex_lock(&GLOBAL_STATE->coinbase_lock);
    coinbase->block_height = result->block_height;
    coinbase->output_count = result->output_count;
    memcpy(coinbase->outputs, result->outputs, sizeof(coinbase->outputs));
    coinbase->value_total_satoshis = result->total_value_satoshis;
    if (result->scriptsig != NULL) {
        snprintf(coinbase->scriptsig, sizeof(coinbase->scriptsig), "%s", result->scriptsig);
    } else {
        coinbase->scriptsig[0] = '\0';
    }
    coinbase->valid = true;
    pthread_mutex_unlock(&GLOBAL_STATE->coinbase_lock);

    ESP_LOGD(TAG, "%s coinbase: block %" PRIu32 ", %d outputs, %" PRIu64 " sats",
             pool_label(pool_id), result->block_height, result->output_count, result->total_value_satoshis);

    free(result->scriptsig);
    free(result);
}

int stratum_get_next_pool_uid(GlobalState *GLOBAL_STATE, int pool_id)
{
    if (pool_id < 0 || pool_id >= POOL_COUNT) {
        return stratum_get_next_uid(GLOBAL_STATE);
    }

    StratumPoolState *pool = &GLOBAL_STATE->pools[pool_id];
    taskENTER_CRITICAL(&pool->mux);
    int uid = pool->send_uid++;
    taskEXIT_CRITICAL(&pool->mux);
    return uid;
}

static void stratum_reset_pool_uid(GlobalState *GLOBAL_STATE, int pool_id)
{
    StratumPoolState *pool = &GLOBAL_STATE->pools[pool_id];
    ESP_LOGI(TAG, "Resetting %s stratum uid", pool_label(pool_id));
    taskENTER_CRITICAL(&pool->mux);
    pool->send_uid = 1;
    taskEXIT_CRITICAL(&pool->mux);
}

static void invalidate_active_pool_jobs(GlobalState *GLOBAL_STATE, int pool_id)
{
    pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock);
    if (GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs != NULL) {
        for (int i = 0; i < 128; i++) {
            bm_job *job = GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[i];
            if (job != NULL && job->pool_id == pool_id) {
                GLOBAL_STATE->valid_jobs[i] = 0;
                GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[i] = NULL;
                free_bm_job(job);
            }
        }
    }
    ASIC_jobs_queue_clear_pool(&GLOBAL_STATE->ASIC_jobs_queue, pool_id);
    pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);

    if (GLOBAL_STATE->ASIC_TASK_MODULE.semaphore != NULL) {
        xSemaphoreGive(GLOBAL_STATE->ASIC_TASK_MODULE.semaphore);
    }
}

static void invalidate_pool_notify(GlobalState *GLOBAL_STATE, int pool_id)
{
    pthread_mutex_lock(&GLOBAL_STATE->stratum_work_lock);
    StratumPoolState *pool = &GLOBAL_STATE->pools[pool_id];
    if (pool->current_notify != NULL) {
        STRATUM_V1_free_mining_notify(pool->current_notify);
        pool->current_notify = NULL;
    }
    pool->valid_notify = false;
    pool->extranonce_2 = 0;
    pthread_cond_broadcast(&GLOBAL_STATE->stratum_work_updated);
    pthread_mutex_unlock(&GLOBAL_STATE->stratum_work_lock);
}

static void invalidate_pool_work(GlobalState *GLOBAL_STATE, int pool_id)
{
    invalidate_pool_notify(GLOBAL_STATE, pool_id);
    invalidate_active_pool_jobs(GLOBAL_STATE, pool_id);
}

bool is_wifi_connected() {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return true;
    } else {
        return false;
    }
}

static bool has_active_jobs(GlobalState * GLOBAL_STATE)
{
    bool has_active = false;

    pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock);
    for (int i = 0; i < 128; i = i + 4) {
        if (GLOBAL_STATE->valid_jobs[i] != 0) {
            has_active = true;
            break;
        }
    }
    pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);

    return has_active;
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

int stratum_get_next_uid(GlobalState * GLOBAL_STATE)
{
    taskENTER_CRITICAL(&GLOBAL_STATE->stratum_mux);
    int uid = GLOBAL_STATE->send_uid++;
    taskEXIT_CRITICAL(&GLOBAL_STATE->stratum_mux);
    return uid;
}

void stratum_reset_uid(GlobalState * GLOBAL_STATE)
{
    ESP_LOGI(TAG, "Resetting stratum uid");
    taskENTER_CRITICAL(&GLOBAL_STATE->stratum_mux);
    GLOBAL_STATE->send_uid = 1;
    taskEXIT_CRITICAL(&GLOBAL_STATE->stratum_mux);
}

// Worker-only: destroys the single-pool transport. Must not be called from any
// task other than stratum_task -- use stratum_request_close() instead.
void stratum_close_connection(GlobalState * GLOBAL_STATE)
{
    ESP_LOGE(TAG, "Shutting down socket and restarting...");

    esp_transport_handle_t transport = conn_claim(&GLOBAL_STATE->transport_lock,
                                                  &GLOBAL_STATE->transport,
                                                  &GLOBAL_STATE->transport_fd);
    GLOBAL_STATE->close_requested = false;

    if (transport != NULL) {
        esp_transport_close(transport);
        esp_transport_destroy(transport);
    }
    cleanQueue(GLOBAL_STATE);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
}

// Asks stratum_task to drop its connection. Safe from any task: it only kicks
// the worker out of its read, and the worker does the destroying.
void stratum_request_close(GlobalState * GLOBAL_STATE)
{
    ESP_LOGI(TAG, "Requesting stratum connection close");
    GLOBAL_STATE->close_requested = true;
    conn_request_close(&GLOBAL_STATE->transport_lock, &GLOBAL_STATE->transport_fd);
}

// Worker-only: destroys this pool's transport. Must not be called from any task
// other than the pool's own worker -- use stratum_request_pool_close() instead.
void stratum_close_pool_connection(GlobalState *GLOBAL_STATE, int pool_id)
{
    if (pool_id < 0 || pool_id >= POOL_COUNT) {
        return;
    }

    StratumPoolState *pool = &GLOBAL_STATE->pools[pool_id];
    ESP_LOGE(TAG, "Shutting down %s pool socket", pool_label(pool_id));

    esp_transport_handle_t transport = conn_claim(&pool->transport_lock,
                                                  &pool->transport,
                                                  &pool->transport_fd);

    taskENTER_CRITICAL(&pool->mux);
    pool->connected = false;
    pool->close_requested = true;
    taskEXIT_CRITICAL(&pool->mux);

    if (transport != NULL) {
        esp_transport_close(transport);
        esp_transport_destroy(transport);
    }

    invalidate_pool_work(GLOBAL_STATE, pool_id);
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// Asks a pool worker to drop its connection. Safe from any task.
void stratum_request_pool_close(GlobalState *GLOBAL_STATE, int pool_id)
{
    if (pool_id < 0 || pool_id >= POOL_COUNT) {
        return;
    }

    StratumPoolState *pool = &GLOBAL_STATE->pools[pool_id];
    ESP_LOGI(TAG, "Requesting %s pool connection close", pool_label(pool_id));

    taskENTER_CRITICAL(&pool->mux);
    pool->close_requested = true;
    taskEXIT_CRITICAL(&pool->mux);

    conn_request_close(&pool->transport_lock, &pool->transport_fd);
}

// Submits a share with the transport pinned for the duration of the write, so
// a concurrent teardown cannot free it mid-send. Returns the byte count from
// the underlying write, a negative errno-style value on write failure, or
// STRATUM_SUBMIT_NO_CONNECTION when the pool is not currently connected.
int stratum_submit_share(GlobalState *GLOBAL_STATE, int pool_id, const char *username,
                         const char *job_id, const char *extranonce_2, uint32_t ntime,
                         uint32_t nonce, uint32_t version_bits)
{
    pthread_mutex_t *lock;
    esp_transport_handle_t *slot;
    int uid;

    if (GLOBAL_STATE->SYSTEM_MODULE.pool_mode == POOL_MODE_DUAL) {
        if (pool_id < 0 || pool_id >= POOL_COUNT) {
            return STRATUM_SUBMIT_NO_CONNECTION;
        }
        StratumPoolState *pool = &GLOBAL_STATE->pools[pool_id];
        lock = &pool->transport_lock;
        slot = &pool->transport;
        uid = stratum_get_next_pool_uid(GLOBAL_STATE, pool_id);
    } else {
        lock = &GLOBAL_STATE->transport_lock;
        slot = &GLOBAL_STATE->transport;
        uid = stratum_get_next_uid(GLOBAL_STATE);
    }

    pthread_mutex_lock(lock);
    int ret = STRATUM_SUBMIT_NO_CONNECTION;
    if (*slot != NULL) {
        ret = STRATUM_V1_submit_share(*slot, uid, username, job_id, extranonce_2,
                                      ntime, nonce, version_bits);
    }
    pthread_mutex_unlock(lock);

    return ret;
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

        stratum_connection_info_t conn_info;
        if (resolve_stratum_address(primary_stratum_url, primary_stratum_port, &conn_info) != ESP_OK) {
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

        if (tls != DISABLED) {
            esp_transport_ssl_set_common_name(transport, primary_stratum_url);
        }
        esp_err_t err = esp_transport_connect(transport, conn_info.host_ip, primary_stratum_port, TRANSPORT_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "Heartbeat. Failed connect check: %s:%d (%s) (errno %d: %s)", primary_stratum_url, primary_stratum_port, conn_info.host_ip, err, strerror(err));
            esp_transport_close(transport);
            esp_transport_destroy(transport);
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
        esp_transport_destroy(transport);

        if (bytes_received == -1)  {
            vTaskDelay(60000 / portTICK_PERIOD_MS);
            continue;
        }

        if (strstr(recv_buffer, "mining.notify") != NULL) {
            ESP_LOGI(TAG, "Heartbeat successful and in fallback mode. Switching back to primary.");
            GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback = false;
            // Only ask: stratum_task owns that transport and is very likely
            // parked inside esp_transport_read() on it right now.
            stratum_request_close(GLOBAL_STATE);
            continue;
        }

        vTaskDelay(60000 / portTICK_PERIOD_MS);
    }
}

static void handle_dual_pool_message(GlobalState *GLOBAL_STATE, int pool_id, StratumApiV1Message *message)
{
    StratumPoolState *pool = &GLOBAL_STATE->pools[pool_id];

    if (message->method == MINING_NOTIFY) {
        SYSTEM_notify_new_ntime(GLOBAL_STATE, message->mining_notification->ntime);

        if (message->mining_notification->clean_jobs) {
            invalidate_pool_work(GLOBAL_STATE, pool_id);
        } else {
            pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock);
            ASIC_jobs_queue_clear_pool(&GLOBAL_STATE->ASIC_jobs_queue, pool_id);
            pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);
        }

        message->mining_notification->difficulty = pool->stratum_difficulty;

        decode_mining_notification(GLOBAL_STATE, pool_id, message->mining_notification,
                                   pool->extranonce_str, pool->extranonce_2_len);

        pthread_mutex_lock(&GLOBAL_STATE->stratum_work_lock);
        if (pool->current_notify != NULL) {
            STRATUM_V1_free_mining_notify(pool->current_notify);
        }
        pool->current_notify = message->mining_notification;
        message->mining_notification = NULL;
        pool->valid_notify = true;
        pool->extranonce_2 = 0;
        pthread_cond_broadcast(&GLOBAL_STATE->stratum_work_updated);
        pthread_mutex_unlock(&GLOBAL_STATE->stratum_work_lock);
    } else if (message->method == MINING_SET_DIFFICULTY) {
        if (message->new_difficulty != pool->stratum_difficulty) {
            pool->stratum_difficulty = message->new_difficulty;
            ESP_LOGI(TAG, "Set %s stratum difficulty: %.2f", pool_label(pool_id), pool->stratum_difficulty);
        }
    } else if (message->method == MINING_SET_VERSION_MASK ||
            message->method == STRATUM_RESULT_VERSION_MASK) {
        ESP_LOGI(TAG, "Set %s version mask: %08lx", pool_label(pool_id), message->version_mask);
        pool->version_mask = message->version_mask;
        pool->new_version_rolling_msg = true;
    } else if (message->method == MINING_SET_EXTRANONCE ||
            message->method == STRATUM_RESULT_SUBSCRIBE) {
        // create_jobs_task reads extranonce_str under stratum_work_lock; the
        // swap and the free have to happen under it too, or that reader can be
        // left holding a pointer we just handed back to the allocator.
        pthread_mutex_lock(&GLOBAL_STATE->stratum_work_lock);
        char *old_extranonce = pool->extranonce_str;
        pool->extranonce_str = message->extranonce_str;
        pool->extranonce_2_len = message->extranonce_2_len;
        message->extranonce_str = NULL;
        pthread_mutex_unlock(&GLOBAL_STATE->stratum_work_lock);
        free(old_extranonce);
    } else if (message->method == MINING_PING) {
        // Pin the transport for the write; we are on the worker task, so this
        // can only contend with a share submit, never with our own teardown.
        pthread_mutex_lock(&pool->transport_lock);
        if (pool->transport != NULL) {
            STRATUM_V1_pong(pool->transport, message->message_id);
        }
        pthread_mutex_unlock(&pool->transport_lock);
    } else if (message->method == CLIENT_RECONNECT) {
        ESP_LOGE(TAG, "%s pool requested client reconnect", pool_label(pool_id));
        stratum_close_pool_connection(GLOBAL_STATE, pool_id);
    } else if (message->method == STRATUM_RESULT) {
        bool is_setup = message->message_id < pool->first_share_uid;
        if (is_setup) {
            if (message->response_success) {
                ESP_LOGI(TAG, "%s setup message accepted", pool_label(pool_id));
            } else {
                ESP_LOGE(TAG, "%s setup message rejected: %s", pool_label(pool_id), message->error_str);
            }
        } else {
            if (message->response_success) {
                ESP_LOGI(TAG, "%s share accepted", pool_label(pool_id));
                SYSTEM_notify_accepted_share(GLOBAL_STATE, pool_id);
            } else {
                ESP_LOGW(TAG, "%s share rejected: %s", pool_label(pool_id), message->error_str);
                SYSTEM_notify_rejected_share(GLOBAL_STATE, pool_id, message->error_str);
            }
        }
    }
}

static void stratum_dual_pool_worker(void *pvParameters)
{
    stratum_pool_task_params_t params = *(stratum_pool_task_params_t *)pvParameters;
    free(pvParameters);

    GlobalState *GLOBAL_STATE = params.state;
    int pool_id = params.pool_id;
    StratumPoolState *pool = &GLOBAL_STATE->pools[pool_id];
    int retry_attempts = 0;
    int retry_critical_attempts = 0;

    while (1) {
        char *stratum_url = pool_url(GLOBAL_STATE, pool_id);
        uint16_t port = pool_port(GLOBAL_STATE, pool_id);
        tls_mode tls = pool_tls(GLOBAL_STATE, pool_id);
        char *cert = pool_cert(GLOBAL_STATE, pool_id);

        if (stratum_url == NULL || stratum_url[0] == '\0') {
            ESP_LOGW(TAG, "No %s pool configured", pool_label(pool_id));
            invalidate_pool_work(GLOBAL_STATE, pool_id);
            vTaskDelay(pdMS_TO_TICKS(60000));
            continue;
        }

        if (!is_wifi_connected()) {
            ESP_LOGI(TAG, "WiFi disconnected, %s pool waiting", pool_label(pool_id));
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        stratum_connection_info_t conn_info;
        if (resolve_stratum_address(stratum_url, port, &conn_info) != ESP_OK) {
            ESP_LOGE(TAG, "DNS resolution failed for %s pool: %s", pool_label(pool_id), stratum_url);
            retry_attempts++;
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        ESP_LOGI(TAG, "Connecting %s pool to: stratum+tcp://%s:%d (%s)", pool_label(pool_id), stratum_url, port, conn_info.host_ip);

        esp_transport_handle_t transport = STRATUM_V1_transport_init(tls, cert);
        if (transport == NULL) {
            ESP_LOGE(TAG, "%s transport initialization failed", pool_label(pool_id));
            if (++retry_critical_attempts > MAX_CRITICAL_RETRY_ATTEMPTS) {
                ESP_LOGE(TAG, "Max retry attempts reached, restarting...");
                esp_restart();
            }
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        retry_critical_attempts = 0;

        if (tls != DISABLED) {
            esp_transport_ssl_set_common_name(transport, stratum_url);
        }

        esp_err_t ret = esp_transport_connect(transport, conn_info.host_ip, port, TRANSPORT_TIMEOUT_MS);
        if (ret != ESP_OK) {
            retry_attempts++;
            ESP_LOGE(TAG, "%s transport unable to connect to %s:%d (errno %d). Attempt: %d", pool_label(pool_id), stratum_url, port, ret, retry_attempts);
            esp_transport_close(transport);
            esp_transport_destroy(transport);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        set_socket_options(transport);
        // Clear the close flag before publishing: a request that lands after
        // this point sets it again and shuts the fd down, so it cannot be lost.
        taskENTER_CRITICAL(&pool->mux);
        pool->connected = true;
        pool->close_requested = false;
        taskEXIT_CRITICAL(&pool->mux);
        conn_publish(&pool->transport_lock, &pool->transport, &pool->transport_fd, transport);

        ESP_LOGI(TAG, "Connected %s pool to %s:%d", pool_label(pool_id), stratum_url, port);

        stratum_reset_pool_uid(GLOBAL_STATE, pool_id);
        invalidate_pool_work(GLOBAL_STATE, pool_id);

        // Held across the whole handshake so a share submit can't interleave
        // its write between our setup messages.
        pthread_mutex_lock(&pool->transport_lock);
        STRATUM_V1_configure_version_rolling(transport, stratum_get_next_pool_uid(GLOBAL_STATE, pool_id), &pool->version_mask);
        STRATUM_V1_subscribe(transport, stratum_get_next_pool_uid(GLOBAL_STATE, pool_id), GLOBAL_STATE->asic_model_str);
        STRATUM_V1_authorize(transport, stratum_get_next_pool_uid(GLOBAL_STATE, pool_id), pool_user(GLOBAL_STATE, pool_id), pool_pass(GLOBAL_STATE, pool_id));

#ifdef CONFIG_STRATUM_SUGGEST_DIFFICULTY
        if (STRATUM_DIFFICULTY > 0) {
            STRATUM_V1_suggest_difficulty(transport, stratum_get_next_pool_uid(GLOBAL_STATE, pool_id), STRATUM_DIFFICULTY);
        }
#endif
        pthread_mutex_unlock(&pool->transport_lock);

        taskENTER_CRITICAL(&pool->mux);
        pool->first_share_uid = pool->send_uid;
        taskEXIT_CRITICAL(&pool->mux);

        StratumApiV1Message message = {0};
        StratumV1RxBuffer rx = {0};
        STRATUM_V1_initialize_rx_buffer(&rx);
        retry_attempts = 0;

        while (1) {
            char *line = STRATUM_V1_receive_jsonrpc_line_ctx(transport, &rx);
            if (!line) {
                ESP_LOGE(TAG, "%s pool failed to receive JSON-RPC line, reconnecting", pool_label(pool_id));
                retry_attempts++;
                stratum_close_pool_connection(GLOBAL_STATE, pool_id);
                break;
            }

            STRATUM_V1_parse(&message, line);
            free(line);
            handle_dual_pool_message(GLOBAL_STATE, pool_id, &message);

            bool closed = false;
            taskENTER_CRITICAL(&pool->mux);
            closed = pool->close_requested;
            taskEXIT_CRITICAL(&pool->mux);

            STRATUM_V1_reset_message(&message);
            if (closed) {
                break;
            }
        }

        STRATUM_V1_reset_message(&message);
        STRATUM_V1_free_rx_buffer(&rx);
        stratum_close_pool_connection(GLOBAL_STATE, pool_id);
    }
}

void stratum_task(void * pvParameters)
{
    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    if (GLOBAL_STATE->SYSTEM_MODULE.pool_mode == POOL_MODE_DUAL) {
        ESP_LOGI(TAG, "Starting dual-pool stratum mode (%u%% primary / %u%% secondary)",
                 GLOBAL_STATE->SYSTEM_MODULE.pool_balance,
                 100 - GLOBAL_STATE->SYSTEM_MODULE.pool_balance);

        for (int i = 0; i < POOL_COUNT; i++) {
            stratum_pool_task_params_t *params = malloc(sizeof(stratum_pool_task_params_t));
            if (params == NULL) {
                ESP_LOGE(TAG, "Failed to allocate %s pool task params", pool_label(i));
                continue;
            }
            params->state = GLOBAL_STATE;
            params->pool_id = i;

            char task_name[24];
            snprintf(task_name, sizeof(task_name), "stratum %s", pool_label(i));
            xTaskCreate(stratum_dual_pool_worker, task_name, 8192, params, 5, NULL);
        }

        vTaskDelete(NULL);
        return;
    }

    primary_stratum_url = GLOBAL_STATE->SYSTEM_MODULE.pool_url;
    primary_stratum_port = GLOBAL_STATE->SYSTEM_MODULE.pool_port;
    primary_stratum_tls = GLOBAL_STATE->SYSTEM_MODULE.pool_tls;
    primary_stratum_cert = GLOBAL_STATE->SYSTEM_MODULE.pool_cert;
    char * stratum_url = GLOBAL_STATE->SYSTEM_MODULE.pool_url;
    uint16_t port = GLOBAL_STATE->SYSTEM_MODULE.pool_port;
    tls_mode tls = GLOBAL_STATE->SYSTEM_MODULE.pool_tls;
    char * cert = GLOBAL_STATE->SYSTEM_MODULE.pool_cert;

    STRATUM_V1_initialize_buffer();
    StratumApiV1Message stratum_api_v1_message = {0};
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

        stratum_connection_info_t conn_info;
        if (resolve_stratum_address(stratum_url, port, &conn_info) != ESP_OK) {
            ESP_LOGE(TAG, "DNS resolution failed for %s", stratum_url);
            retry_attempts++;
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI(TAG, "Connecting to: stratum+tcp://%s:%d (%s)", stratum_url, port, conn_info.host_ip);

        tls = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_tls : GLOBAL_STATE->SYSTEM_MODULE.pool_tls;
        cert = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_cert : GLOBAL_STATE->SYSTEM_MODULE.pool_cert;

        // Kept local until the connection is up: publishing early would let
        // ASIC_result_task write shares into a transport that isn't connected.
        esp_transport_handle_t transport = STRATUM_V1_transport_init(tls, cert);
        if (transport == NULL) {
            ESP_LOGE(TAG, "Transport initialization failed.");
            if (++retry_critical_attempts > MAX_CRITICAL_RETRY_ATTEMPTS) {
                ESP_LOGE(TAG, "Max retry attempts reached, restarting...");
                esp_restart();
            }
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            continue;
        }
        retry_critical_attempts = 0;

        if (tls != DISABLED) {
            esp_transport_ssl_set_common_name(transport, stratum_url);
        }
        ESP_LOGI(TAG, "Transport initialized, connecting to %s:%d (%s)", stratum_url, port, conn_info.host_ip);
        esp_err_t ret = esp_transport_connect(transport, conn_info.host_ip, port, TRANSPORT_TIMEOUT_MS);
        if (ret != ESP_OK) {
            retry_attempts++;
            ESP_LOGE(TAG, "Transport unable to connect to %s:%d (errno %d). Attempt: %d", stratum_url, port, ret, retry_attempts);
            esp_transport_close(transport);
            esp_transport_destroy(transport);
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            continue;
        }

        set_socket_options(transport);
        GLOBAL_STATE->close_requested = false;
        conn_publish(&GLOBAL_STATE->transport_lock, &GLOBAL_STATE->transport,
                     &GLOBAL_STATE->transport_fd, transport);

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

        char * username = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_user : GLOBAL_STATE->SYSTEM_MODULE.pool_user;
        char * password = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_pass : GLOBAL_STATE->SYSTEM_MODULE.pool_pass;

        ///// Start Stratum Action
        // Held across the whole handshake so a share submit can't interleave
        // its write between our setup messages.
        pthread_mutex_lock(&GLOBAL_STATE->transport_lock);

        // mining.configure - ID: 1
        STRATUM_V1_configure_version_rolling(transport, stratum_get_next_uid(GLOBAL_STATE), &GLOBAL_STATE->version_mask);

        // mining.subscribe - ID: 2
        STRATUM_V1_subscribe(transport, stratum_get_next_uid(GLOBAL_STATE), GLOBAL_STATE->asic_model_str);

        //mining.authorize - ID: 3
        STRATUM_V1_authorize(transport, stratum_get_next_uid(GLOBAL_STATE), username, password);

#ifdef CONFIG_STRATUM_SUGGEST_DIFFICULTY
        if (STRATUM_DIFFICULTY > 0) {
            // mining.suggest_difficulty - optional; some pools reject this method.
            STRATUM_V1_suggest_difficulty(transport, stratum_get_next_uid(GLOBAL_STATE), STRATUM_DIFFICULTY);
        }
#endif
        pthread_mutex_unlock(&GLOBAL_STATE->transport_lock);

        // The next uid that will be issued belongs to a share submission.
        // Used by the dispatcher below to distinguish setup-message replies
        // from share replies without relying on a hardcoded id threshold.
        taskENTER_CRITICAL(&GLOBAL_STATE->stratum_mux);
        int first_share_uid = GLOBAL_STATE->send_uid;
        taskEXIT_CRITICAL(&GLOBAL_STATE->stratum_mux);

        // Everything is set up, lets make sure we don't abandon work unnecessarily.
        GLOBAL_STATE->abandon_work = 0;

        while (1) {
            // `transport` is this task's own handle: we are the only task that
            // may destroy it, so it cannot go away while we are parked in here.
            char * line = STRATUM_V1_receive_jsonrpc_line(transport);
            if (!line) {
                ESP_LOGE(TAG, "Failed to receive JSON-RPC line, reconnecting...");
                retry_attempts++;
                stratum_close_connection(GLOBAL_STATE);
                break;
            }

            if (GLOBAL_STATE->close_requested) {
                ESP_LOGI(TAG, "Close requested, dropping connection");
                free(line);
                stratum_close_connection(GLOBAL_STATE);
                break;
            }

            STRATUM_V1_parse(&stratum_api_v1_message, line);
            free(line);

            if (stratum_api_v1_message.method == MINING_NOTIFY) {
                SYSTEM_notify_new_ntime(GLOBAL_STATE, stratum_api_v1_message.mining_notification->ntime);
                bool stratum_queue_has_work = queue_count(&GLOBAL_STATE->stratum_queue) > 0;
                bool asic_queue_has_work = queue_count(&GLOBAL_STATE->ASIC_jobs_queue) > 0;
                if (stratum_api_v1_message.mining_notification->clean_jobs) {
                    if (stratum_queue_has_work || asic_queue_has_work || has_active_jobs(GLOBAL_STATE)) {
                        cleanQueue(GLOBAL_STATE);
                    }
                } else if (asic_queue_has_work) {
                    ESP_LOGI(TAG, "New notify: dropping pending ASIC jobs from older work");
                    pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock);
                    ASIC_jobs_queue_clear(&GLOBAL_STATE->ASIC_jobs_queue);
                    pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);
                }
                void *next_notify = NULL;
                if (queue_dequeue_if_full(&GLOBAL_STATE->stratum_queue, &next_notify)) {
                    mining_notify * next_notify_json_str = (mining_notify *) next_notify;
                    STRATUM_V1_free_mining_notify(next_notify_json_str);
                }
                stratum_api_v1_message.mining_notification->difficulty = SYSTEM_TASK_MODULE.stratum_difficulty;
                decode_mining_notification(GLOBAL_STATE,
                                           GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? POOL_SECONDARY : POOL_PRIMARY,
                                           stratum_api_v1_message.mining_notification,
                                           GLOBAL_STATE->extranonce_str, GLOBAL_STATE->extranonce_2_len);
                queue_enqueue(&GLOBAL_STATE->stratum_queue, stratum_api_v1_message.mining_notification);
                stratum_api_v1_message.mining_notification = NULL;
            } else if (stratum_api_v1_message.method == MINING_SET_DIFFICULTY) {
                if (stratum_api_v1_message.new_difficulty != SYSTEM_TASK_MODULE.stratum_difficulty) {
                    SYSTEM_TASK_MODULE.stratum_difficulty = stratum_api_v1_message.new_difficulty;
                    ESP_LOGI(TAG, "Set stratum difficulty: %.2f", SYSTEM_TASK_MODULE.stratum_difficulty);
                }
            } else if (stratum_api_v1_message.method == MINING_SET_VERSION_MASK ||
                    stratum_api_v1_message.method == STRATUM_RESULT_VERSION_MASK) {
                ESP_LOGI(TAG, "Set version mask: %08lx", stratum_api_v1_message.version_mask);
                GLOBAL_STATE->version_mask = stratum_api_v1_message.version_mask;
                GLOBAL_STATE->new_stratum_version_rolling_msg = true;
            } else if (stratum_api_v1_message.method == MINING_SET_EXTRANONCE ||
                    stratum_api_v1_message.method == STRATUM_RESULT_SUBSCRIBE) {
                // create_jobs_task snapshots extranonce_str under
                // stratum_work_lock; the swap and the free have to happen under
                // it too, or that reader can be left holding a pointer we just
                // handed back to the allocator.
                pthread_mutex_lock(&GLOBAL_STATE->stratum_work_lock);
                char *old_extranonce = GLOBAL_STATE->extranonce_str;
                GLOBAL_STATE->extranonce_str = stratum_api_v1_message.extranonce_str;
                GLOBAL_STATE->extranonce_2_len = stratum_api_v1_message.extranonce_2_len;
                stratum_api_v1_message.extranonce_str = NULL;
                pthread_mutex_unlock(&GLOBAL_STATE->stratum_work_lock);
                free(old_extranonce);
            } else if (stratum_api_v1_message.method == MINING_PING) {
                // Pin the transport for the write; we are the owning task, so
                // this can only contend with a share submit.
                pthread_mutex_lock(&GLOBAL_STATE->transport_lock);
                if (GLOBAL_STATE->transport != NULL) {
                    STRATUM_V1_pong(GLOBAL_STATE->transport, stratum_api_v1_message.message_id);
                }
                pthread_mutex_unlock(&GLOBAL_STATE->transport_lock);
            } else if (stratum_api_v1_message.method == CLIENT_RECONNECT) {
                ESP_LOGE(TAG, "Pool requested client reconnect...");
                stratum_close_connection(GLOBAL_STATE);
                break;
            } else if (stratum_api_v1_message.method == STRATUM_RESULT) {
                bool is_setup = stratum_api_v1_message.message_id < first_share_uid;
                if (is_setup) {
                    // Reset retry attempts after successfully receiving setup data.
                    retry_attempts = 0;
                    if (stratum_api_v1_message.response_success) {
                        ESP_LOGI(TAG, "setup message accepted");
                    } else {
                        ESP_LOGE(TAG, "setup message rejected: %s", stratum_api_v1_message.error_str);
                    }
                } else {
                    if (stratum_api_v1_message.response_success) {
                        ESP_LOGI(TAG, "message result accepted");
                        SYSTEM_notify_accepted_share(GLOBAL_STATE, GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? POOL_SECONDARY : POOL_PRIMARY);
                    } else {
                        ESP_LOGW(TAG, "message result rejected: %s", stratum_api_v1_message.error_str);
                        SYSTEM_notify_rejected_share(GLOBAL_STATE, GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? POOL_SECONDARY : POOL_PRIMARY, stratum_api_v1_message.error_str);
                    }
                }
            }

            STRATUM_V1_reset_message(&stratum_api_v1_message);
        }

        STRATUM_V1_reset_message(&stratum_api_v1_message);
    }
    vTaskDelete(NULL);
}
