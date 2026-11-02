#include "esp_log.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include <lwip/sockets.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "global_state.h"
#include "system.h"
#include "asic.h"
#include "stratum_task.h"
#include "stratum_v2_client.h"
#include "sv2_protocol.h"
#include "sv2_noise.h"
#include "libbase58.h"
#include "utils.h"
#include "job_id_filter.h"

static const char *TAG = "stratum_v2";

#define SV2_VENDOR "bitforge"
#define SV2_PROBE_BUFFER_SIZE 1024
// Future jobs (no min_ntime) waiting for the SetNewPrevHash that activates them.
#define SV2_PENDING_JOBS 8
#define MIN_POOL_DIFFICULTY 0.0001
#define MAX_POOL_DIFFICULTY 4294967295.0

typedef struct {
    sv2_noise_ctx_t *noise_ctx;
    int pool_id;
    sv2_channel_type_t channel_type;
    uint32_t channel_id;
    uint32_t group_channel_id;
    uint32_t sequence_number;      // also the count of shares submitted
    uint32_t resolved_shares;      // shares the pool has accepted or rejected
    uint8_t target[32];

    uint8_t prev_hash[32];
    uint32_t prev_hash_nbits;
    bool has_prev_hash;

    // Extended channels only; extranonce_hex is the prefix as the V1-style
    // extranonce1 string create_jobs_task builds the coinbase from.
    uint8_t extranonce_size;
    char extranonce_hex[SV2_MAX_EXTRANONCE_SIZE * 2 + 1];

    mining_notify *pending[SV2_PENDING_JOBS];
    uint32_t pending_job_id[SV2_PENDING_JOBS];
    int pending_next;

    // Job ids already handed to the miner since the last SetNewPrevHash, so
    // a repeated job isn't mined twice. Zeroed (= empty) by the calloc.
    job_id_filter_t job_filter;
} sv2_conn_t;

// The live session, published together with GLOBAL_STATE->transport under
// transport_lock so a share submit always sees a matching pair.
static sv2_conn_t *s_conn = NULL;

static char *pool_url(GlobalState *GLOBAL_STATE, int pool_id)
{
    return pool_id == POOL_SECONDARY ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_url : GLOBAL_STATE->SYSTEM_MODULE.pool_url;
}

static uint16_t pool_port(GlobalState *GLOBAL_STATE, int pool_id)
{
    return pool_id == POOL_SECONDARY ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_port : GLOBAL_STATE->SYSTEM_MODULE.pool_port;
}

static char *pool_user(GlobalState *GLOBAL_STATE, int pool_id)
{
    return pool_id == POOL_SECONDARY ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_user : GLOBAL_STATE->SYSTEM_MODULE.pool_user;
}

static sv2_channel_type_t pool_channel_type(GlobalState *GLOBAL_STATE, int pool_id)
{
    return pool_id == POOL_SECONDARY ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_sv2_channel_type
                                     : GLOBAL_STATE->SYSTEM_MODULE.pool_sv2_channel_type;
}

static char *pool_authority_pubkey(GlobalState *GLOBAL_STATE, int pool_id)
{
    return pool_id == POOL_SECONDARY ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_sv2_authority_pubkey
                                     : GLOBAL_STATE->SYSTEM_MODULE.pool_sv2_authority_pubkey;
}

static bool pool_require_auth(GlobalState *GLOBAL_STATE, int pool_id)
{
    return pool_id == POOL_SECONDARY ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_sv2_require_auth
                                     : GLOBAL_STATE->SYSTEM_MODULE.pool_sv2_require_auth;
}

static void set_connection_info(GlobalState *GLOBAL_STATE, const char *info)
{
    snprintf(GLOBAL_STATE->SYSTEM_MODULE.pool_connection_info,
             sizeof(GLOBAL_STATE->SYSTEM_MODULE.pool_connection_info), "%s", info);
}

// Authority keys are Base58Check: 2-byte version (0x0001 LE) + 32-byte x-only
// pubkey + 4-byte checksum.
static bool load_authority_pubkey(uint8_t out[SV2_AUTHORITY_PUBKEY_SIZE], const char *b58_key)
{
    if (b58_key == NULL || b58_key[0] == '\0') {
        return false;
    }

    uint8_t decoded[64];
    size_t decoded_len = sizeof(decoded);
    if (!b58tobin(decoded, &decoded_len, b58_key, 0)) {
        ESP_LOGE(TAG, "Failed to decode base58 authority pubkey");
        return false;
    }
    if (decoded_len != SV2_BASE58_AUTHORITY_KEY_LEN) {
        ESP_LOGE(TAG, "Invalid authority pubkey length: %zu (expected %d)", decoded_len, SV2_BASE58_AUTHORITY_KEY_LEN);
        return false;
    }

    // b58tobin right-aligns its output in the buffer
    const uint8_t *data = decoded + (sizeof(decoded) - decoded_len);
    if (data[0] != 0x01 || data[1] != 0x00) {
        ESP_LOGE(TAG, "Invalid authority pubkey version: 0x%02x%02x (expected 0x0001)", data[1], data[0]);
        return false;
    }

    uint8_t checksum[32];
    uint8_t *hash = double_sha256_bin(data, SV2_BASE58_AUTHORITY_KEY_LEN - 4);
    if (hash == NULL) {
        return false;
    }
    memcpy(checksum, hash, sizeof(checksum));
    free(hash);
    if (memcmp(checksum, data + SV2_BASE58_AUTHORITY_KEY_LEN - 4, 4) != 0) {
        ESP_LOGE(TAG, "Authority pubkey checksum mismatch");
        return false;
    }

    memcpy(out, data + 2, SV2_AUTHORITY_PUBKEY_SIZE);
    return true;
}

static float nominal_hash_rate(GlobalState *GLOBAL_STATE)
{
    float gh = GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value *
               ASIC_get_small_core_count(GLOBAL_STATE) * ASIC_get_asic_count(GLOBAL_STATE) / 1000.0f;
    return gh > 0.0f ? gh * 1e9f : 1e12f;
}

static char *prev_hash_to_v1_hex(const uint8_t prev_hash[32])
{
    char *hex = malloc(65);
    if (hex != NULL) {
        sv2_prev_hash_to_stratum_hex(prev_hash, hex);
    }
    return hex;
}

static char *bytes_to_hex(const uint8_t *data, size_t len)
{
    char *hex = malloc(len * 2 + 1);
    if (hex != NULL) {
        bin2hex(data, len, hex, len * 2 + 1);
    }
    return hex;
}

static void clear_pending_jobs(sv2_conn_t *conn)
{
    for (int i = 0; i < SV2_PENDING_JOBS; i++) {
        STRATUM_V1_free_mining_notify(conn->pending[i]);
        conn->pending[i] = NULL;
    }
    conn->pending_next = 0;
}

static void store_pending_job(sv2_conn_t *conn, uint32_t job_id, mining_notify *notify)
{
    int slot = conn->pending_next;
    STRATUM_V1_free_mining_notify(conn->pending[slot]);
    conn->pending[slot] = notify;
    conn->pending_job_id[slot] = job_id;
    conn->pending_next = (slot + 1) % SV2_PENDING_JOBS;
}

static mining_notify *take_pending_job(sv2_conn_t *conn, uint32_t job_id)
{
    for (int i = 0; i < SV2_PENDING_JOBS; i++) {
        if (conn->pending[i] != NULL && conn->pending_job_id[i] == job_id) {
            mining_notify *notify = conn->pending[i];
            conn->pending[i] = NULL;
            return notify;
        }
    }
    return NULL;
}

// Completes a job with the current prev_hash and hands it to the mining
// pipeline. Takes ownership of notify.
static void activate_job(GlobalState *GLOBAL_STATE, sv2_conn_t *conn, mining_notify *notify,
                         uint32_t ntime, bool clean_jobs)
{
    if (!job_id_filter_accept(&conn->job_filter, notify->job_id, clean_jobs)) {
        ESP_LOGW(TAG, "Ignoring duplicate V2 job %s", notify->job_id);
        STRATUM_V1_free_mining_notify(notify);
        return;
    }

    free(notify->prev_block_hash);
    notify->prev_block_hash = prev_hash_to_v1_hex(conn->prev_hash);
    if (notify->prev_block_hash == NULL) {
        ESP_LOGE(TAG, "Out of memory activating job %s", notify->job_id);
        STRATUM_V1_free_mining_notify(notify);
        return;
    }
    notify->target = conn->prev_hash_nbits;
    notify->ntime = ntime;
    notify->clean_jobs = clean_jobs;
    notify->difficulty = target_to_diff(conn->target);

    ESP_LOGI(TAG, "New job %s (%s)", notify->job_id, clean_jobs ? "new block" : "update");
    stratum_enqueue_notify(GLOBAL_STATE, conn->pool_id, notify,
                           conn->channel_type == SV2_CHANNEL_EXTENDED ? conn->extranonce_hex : NULL,
                           conn->extranonce_size);
}

static mining_notify *new_job_notify(uint32_t job_id, uint32_t version)
{
    mining_notify *notify = calloc(1, sizeof(mining_notify));
    if (notify == NULL) {
        return NULL;
    }
    notify->protocol = STRATUM_PROTOCOL_V2;
    notify->version = version;
    notify->job_id = malloc(11);
    if (notify->job_id == NULL) {
        free(notify);
        return NULL;
    }
    snprintf(notify->job_id, 11, "%" PRIu32, job_id);
    return notify;
}

static void dispatch_new_job(GlobalState *GLOBAL_STATE, sv2_conn_t *conn, uint32_t job_id,
                             mining_notify *notify, bool has_min_ntime, uint32_t min_ntime)
{
    if (!has_min_ntime) {
        // Future job: mined only once a SetNewPrevHash names it.
        store_pending_job(conn, job_id, notify);
        return;
    }
    if (!conn->has_prev_hash) {
        ESP_LOGW(TAG, "Dropping job %" PRIu32 " received before any SetNewPrevHash", job_id);
        STRATUM_V1_free_mining_notify(notify);
        return;
    }
    activate_job(GLOBAL_STATE, conn, notify, min_ntime, false);
}

static bool handle_new_extended_mining_job(GlobalState *GLOBAL_STATE, sv2_conn_t *conn,
                                           const uint8_t *payload, uint32_t len)
{
    sv2_extended_job_t job;
    if (sv2_parse_new_extended_mining_job(payload, len, &job) != 0) {
        ESP_LOGE(TAG, "Failed to parse NewExtendedMiningJob");
        return false;
    }
    if (!sv2_channel_or_group_matches(job.channel_id, conn->channel_id, conn->group_channel_id)) {
        ESP_LOGW(TAG, "Dropping NewExtendedMiningJob for unexpected channel %" PRIu32, job.channel_id);
        return false;
    }

    mining_notify *notify = new_job_notify(job.job_id, job.version);
    if (notify == NULL) {
        ESP_LOGE(TAG, "Out of memory for job %" PRIu32, job.job_id);
        return false;
    }
    notify->no_version_rolling = !job.version_rolling_allowed;
    notify->coinbase_1 = bytes_to_hex(job.coinbase_prefix, job.coinbase_prefix_len);
    notify->coinbase_2 = bytes_to_hex(job.coinbase_suffix, job.coinbase_suffix_len);
    notify->n_merkle_branches = job.merkle_path_count;
    if (job.merkle_path_count > 0) {
        notify->merkle_branches = malloc((size_t)job.merkle_path_count * 32);
        if (notify->merkle_branches != NULL) {
            memcpy(notify->merkle_branches, job.merkle_path, (size_t)job.merkle_path_count * 32);
        }
    }
    if (notify->coinbase_1 == NULL || notify->coinbase_2 == NULL ||
        (job.merkle_path_count > 0 && notify->merkle_branches == NULL)) {
        ESP_LOGE(TAG, "Out of memory for job %" PRIu32, job.job_id);
        STRATUM_V1_free_mining_notify(notify);
        return false;
    }

    dispatch_new_job(GLOBAL_STATE, conn, job.job_id, notify, job.has_min_ntime, job.min_ntime);
    return true;
}

static bool handle_new_mining_job(GlobalState *GLOBAL_STATE, sv2_conn_t *conn,
                                  const uint8_t *payload, uint32_t len)
{
    uint32_t channel_id, job_id, version, min_ntime;
    uint8_t merkle_root[32];
    bool has_min_ntime = false;

    if (sv2_parse_new_mining_job(payload, len, &channel_id, &job_id, &has_min_ntime,
                                 &min_ntime, &version, merkle_root) != 0) {
        ESP_LOGE(TAG, "Failed to parse NewMiningJob");
        return false;
    }
    if (!sv2_channel_or_group_matches(channel_id, conn->channel_id, conn->group_channel_id)) {
        ESP_LOGW(TAG, "Dropping NewMiningJob for unexpected channel %" PRIu32, channel_id);
        return false;
    }

    mining_notify *notify = new_job_notify(job_id, version);
    if (notify == NULL || (notify->merkle_root = bytes_to_hex(merkle_root, 32)) == NULL) {
        ESP_LOGE(TAG, "Out of memory for job %" PRIu32, job_id);
        STRATUM_V1_free_mining_notify(notify);
        return false;
    }

    dispatch_new_job(GLOBAL_STATE, conn, job_id, notify, has_min_ntime, min_ntime);
    return true;
}

static bool handle_set_new_prev_hash(GlobalState *GLOBAL_STATE, sv2_conn_t *conn,
                                     const uint8_t *payload, uint32_t len)
{
    uint32_t channel_id, job_id, min_ntime, nbits;
    uint8_t prev_hash[32];

    if (sv2_parse_set_new_prev_hash(payload, len, &channel_id, &job_id, prev_hash, &min_ntime, &nbits) != 0) {
        ESP_LOGE(TAG, "Failed to parse SetNewPrevHash");
        return false;
    }
    if (nbits == 0) {
        ESP_LOGW(TAG, "Ignoring SetNewPrevHash with zero nbits");
        return false;
    }
    if (!sv2_channel_or_group_matches(channel_id, conn->channel_id, conn->group_channel_id)) {
        ESP_LOGW(TAG, "Dropping SetNewPrevHash for unexpected channel %" PRIu32, channel_id);
        return false;
    }

    memcpy(conn->prev_hash, prev_hash, 32);
    conn->prev_hash_nbits = nbits;
    conn->has_prev_hash = true;

    mining_notify *notify = take_pending_job(conn, job_id);
    // Every other future job was built for a block that is now stale.
    clear_pending_jobs(conn);
    if (notify == NULL) {
        ESP_LOGW(TAG, "SetNewPrevHash for unknown job %" PRIu32, job_id);
        return false;
    }

    activate_job(GLOBAL_STATE, conn, notify, min_ntime, true);
    return true;
}

static void handle_set_target(sv2_conn_t *conn, const uint8_t *payload, uint32_t len)
{
    uint32_t channel_id;
    uint8_t max_target[32];

    if (sv2_parse_set_target(payload, len, &channel_id, max_target) != 0) {
        ESP_LOGE(TAG, "Failed to parse SetTarget");
        return;
    }
    if (!sv2_channel_or_group_matches(channel_id, conn->channel_id, conn->group_channel_id)) {
        ESP_LOGW(TAG, "Dropping SetTarget for unexpected channel %" PRIu32, channel_id);
        return;
    }

    double pdiff = target_to_diff(max_target);
    if (pdiff < MIN_POOL_DIFFICULTY || pdiff > MAX_POOL_DIFFICULTY) {
        ESP_LOGW(TAG, "Ignoring out-of-range SV2 target, difficulty %g", pdiff);
        return;
    }

    memcpy(conn->target, max_target, 32);
    ESP_LOGI(TAG, "Set pool difficulty: %g", pdiff);
}

static void handle_submit_shares_success(GlobalState *GLOBAL_STATE, sv2_conn_t *conn,
                                         const uint8_t *payload, uint32_t len)
{
    uint32_t channel_id, last_sequence_number, accepted_count;
    if (sv2_parse_submit_shares_success(payload, len, &channel_id, &last_sequence_number, &accepted_count) != 0) {
        ESP_LOGE(TAG, "Failed to parse SubmitSharesSuccess");
        return;
    }
    if (channel_id != conn->channel_id) {
        ESP_LOGW(TAG, "Dropping SubmitSharesSuccess for unexpected channel %" PRIu32, channel_id);
        return;
    }

    // A pool can batch acknowledgements; never count more than is outstanding.
    uint32_t pending = conn->sequence_number > conn->resolved_shares ? conn->sequence_number - conn->resolved_shares : 0;
    if (accepted_count > pending) {
        accepted_count = pending;
    }
    if (last_sequence_number + 1 > conn->resolved_shares) {
        conn->resolved_shares = last_sequence_number + 1;
    }
    ESP_LOGI(TAG, "%s shares accepted: %" PRIu32, stratum_pool_label(conn->pool_id), accepted_count);
    for (uint32_t i = 0; i < accepted_count; i++) {
        SYSTEM_notify_accepted_share(GLOBAL_STATE, conn->pool_id);
    }
}

static void handle_submit_shares_error(GlobalState *GLOBAL_STATE, sv2_conn_t *conn,
                                       const uint8_t *payload, uint32_t len)
{
    uint32_t channel_id, seq_num;
    char error_code[64];
    if (sv2_parse_submit_shares_error(payload, len, &channel_id, &seq_num, error_code, sizeof(error_code)) != 0) {
        ESP_LOGE(TAG, "Failed to parse SubmitSharesError");
        return;
    }
    if (channel_id != conn->channel_id) {
        ESP_LOGW(TAG, "Dropping SubmitSharesError for unexpected channel %" PRIu32, channel_id);
        return;
    }
    if (seq_num + 1 > conn->resolved_shares) {
        conn->resolved_shares = seq_num + 1;
    }
    ESP_LOGW(TAG, "%s share rejected: %s", stratum_pool_label(conn->pool_id), error_code);
    SYSTEM_notify_rejected_share(GLOBAL_STATE, conn->pool_id, error_code);
}

int stratum_v2_submit_share_locked(GlobalState *GLOBAL_STATE, esp_transport_handle_t transport,
                                   const char *job_id, const char *extranonce_2,
                                   uint32_t ntime, uint32_t nonce, uint32_t rolled_version)
{
    sv2_conn_t *conn = s_conn;
    if (conn == NULL || conn->noise_ctx == NULL) {
        return STRATUM_SUBMIT_NO_CONNECTION;
    }

    uint8_t extranonce[SV2_MAX_EXTRANONCE_SIZE];
    uint8_t extranonce_len = 0;
    if (conn->channel_type == SV2_CHANNEL_EXTENDED) {
        size_t hex_len = extranonce_2 ? strlen(extranonce_2) : 0;
        if (hex_len == 0 || hex_len / 2 != conn->extranonce_size) {
            ESP_LOGW(TAG, "Dropping share with %zu byte extranonce, channel expects %u", hex_len / 2, conn->extranonce_size);
            return STRATUM_SUBMIT_NO_CONNECTION;
        }
        extranonce_len = conn->extranonce_size;
        hex2bin(extranonce_2, extranonce, extranonce_len);
    }

    uint8_t frame[SV2_SUBMIT_SHARES_MAX_FRAME_SIZE];
    int len = sv2_build_submit_shares(frame, sizeof(frame), conn->channel_id, conn->sequence_number,
                                      (uint32_t)strtoul(job_id, NULL, 10), nonce, ntime, rolled_version,
                                      extranonce_len > 0 ? extranonce : NULL, extranonce_len);
    if (len < 0) {
        return STRATUM_SUBMIT_NO_CONNECTION;
    }

    if (sv2_noise_send(conn->noise_ctx, transport, frame, len) != 0) {
        return -1;
    }
    conn->sequence_number++;
    return len;
}

static void publish_session(GlobalState *GLOBAL_STATE, esp_transport_handle_t transport, sv2_conn_t *conn)
{
    pthread_mutex_lock(&GLOBAL_STATE->transport_lock);
    GLOBAL_STATE->transport = transport;
    GLOBAL_STATE->transport_fd = esp_transport_get_socket(transport);
    s_conn = conn;
    pthread_mutex_unlock(&GLOBAL_STATE->transport_lock);
}

// Takes the session out of the shared slot (so no submit can reach it any
// more) and frees it. Worker-only.
static void teardown_session(GlobalState *GLOBAL_STATE, esp_transport_handle_t transport, sv2_conn_t *conn)
{
    pthread_mutex_lock(&GLOBAL_STATE->transport_lock);
    if (GLOBAL_STATE->transport == transport) {
        GLOBAL_STATE->transport = NULL;
        GLOBAL_STATE->transport_fd = -1;
    }
    if (s_conn == conn) {
        s_conn = NULL;
    }
    pthread_mutex_unlock(&GLOBAL_STATE->transport_lock);

    if (transport != NULL) {
        esp_transport_close(transport);
        esp_transport_destroy(transport);
    }
    if (conn != NULL) {
        clear_pending_jobs(conn);
        sv2_noise_destroy(conn->noise_ctx);
        free(conn);
    }
}

// TCP connect + Noise handshake. On success returns the transport and the
// established noise context; on failure cleans up and returns NULL.
static esp_transport_handle_t connect_encrypted(GlobalState *GLOBAL_STATE, int pool_id,
                                                sv2_noise_ctx_t **noise_out, bool log)
{
    const char *url = pool_url(GLOBAL_STATE, pool_id);
    uint16_t port = pool_port(GLOBAL_STATE, pool_id);
    *noise_out = NULL;

    if (url == NULL || url[0] == '\0' || port == 0) {
        return NULL;
    }

    uint8_t auth_key[SV2_AUTHORITY_PUBKEY_SIZE];
    bool has_auth = load_authority_pubkey(auth_key, pool_authority_pubkey(GLOBAL_STATE, pool_id));
    if (pool_require_auth(GLOBAL_STATE, pool_id) && !has_auth) {
        if (log) {
            ESP_LOGE(TAG, "Authentication required but no valid authority pubkey configured, refusing to connect");
            set_connection_info(GLOBAL_STATE, "SV2: Auth required - no key");
        }
        return NULL;
    }

    stratum_connection_info_t conn_info;
    if (stratum_resolve_address(url, port, &conn_info) != ESP_OK) {
        if (log) set_connection_info(GLOBAL_STATE, "SV2: Pool unreachable");
        return NULL;
    }

    if (log) {
        ESP_LOGI(TAG, "Connecting %s pool to: stratum2+tcp://%s:%u (%s)", stratum_pool_label(pool_id), url, port, conn_info.host_ip);
    }

    esp_transport_handle_t transport = esp_transport_tcp_init();
    if (transport == NULL) {
        ESP_LOGE(TAG, "Failed to init TCP transport");
        return NULL;
    }

    if (esp_transport_connect(transport, conn_info.host_ip, port, STRATUM_TRANSPORT_TIMEOUT_MS) != ESP_OK) {
        if (log) {
            ESP_LOGE(TAG, "TCP connect failed to %s:%u (%s)", url, port, conn_info.host_ip);
            set_connection_info(GLOBAL_STATE, "SV2: Pool unreachable");
        }
        esp_transport_close(transport);
        esp_transport_destroy(transport);
        return NULL;
    }
    stratum_set_socket_options(transport);

    sv2_noise_ctx_t *noise = sv2_noise_create();
    if (noise == NULL) {
        ESP_LOGE(TAG, "Failed to create noise context");
        esp_transport_close(transport);
        esp_transport_destroy(transport);
        return NULL;
    }

    if (log && !has_auth) {
        ESP_LOGW(TAG, "No authority pubkey configured, pool identity will not be verified");
    }

    if (sv2_noise_handshake(noise, transport, has_auth ? auth_key : NULL) != 0) {
        if (log) {
            ESP_LOGE(TAG, "Noise handshake failed");
            set_connection_info(GLOBAL_STATE, has_auth ? "SV2: Auth failed - check key" : "SV2: Handshake failed");
        }
        sv2_noise_destroy(noise);
        esp_transport_close(transport);
        esp_transport_destroy(transport);
        return NULL;
    }

    if (log) {
        set_connection_info(GLOBAL_STATE, conn_info.addr_family == AF_INET6 ? "SV2 IPv6" : "SV2 IPv4");
    }
    *noise_out = noise;
    return transport;
}

// Sends SetupConnection and waits for its success reply.
static bool setup_connection(GlobalState *GLOBAL_STATE, int pool_id, sv2_noise_ctx_t *noise,
                             esp_transport_handle_t transport, sv2_channel_type_t channel_type,
                             uint8_t *frame_buf, uint8_t *recv_buf, int buf_size, bool log)
{
    const char *firmware = esp_app_get_description()->version;
    int frame_len = sv2_build_setup_connection(frame_buf, buf_size,
                                               pool_url(GLOBAL_STATE, pool_id), pool_port(GLOBAL_STATE, pool_id),
                                               SV2_VENDOR, GLOBAL_STATE->asic_model_str ? GLOBAL_STATE->asic_model_str : "",
                                               firmware, "", sv2_setup_flags_for_channel(channel_type));
    if (frame_len < 0 || sv2_noise_send(noise, transport, frame_buf, frame_len) != 0) {
        if (log) ESP_LOGE(TAG, "Failed to send SetupConnection");
        return false;
    }

    uint8_t hdr_buf[SV2_FRAME_HEADER_SIZE];
    int payload_len = 0;
    if (sv2_noise_recv(noise, transport, hdr_buf, recv_buf, buf_size, &payload_len) != 0) {
        if (log) ESP_LOGE(TAG, "No reply to SetupConnection");
        return false;
    }

    sv2_frame_header_t hdr;
    sv2_parse_frame_header(hdr_buf, &hdr);
    if (hdr.msg_type != SV2_MSG_SETUP_CONNECTION_SUCCESS) {
        if (log) {
            ESP_LOGE(TAG, "SetupConnection rejected by pool (msg_type=0x%02x)", hdr.msg_type);
            set_connection_info(GLOBAL_STATE, "SV2: Pool rejected config");
        }
        return false;
    }

    uint16_t used_version;
    uint32_t flags;
    if (sv2_parse_setup_connection_success(recv_buf, payload_len, &used_version, &flags) != 0) {
        if (log) ESP_LOGE(TAG, "Failed to parse SetupConnection.Success");
        return false;
    }
    if (!sv2_setup_success_allows_version_rolling(flags)) {
        // The BM1370 always rolls version bits.
        if (log) {
            ESP_LOGE(TAG, "Pool requires a fixed block version, which this miner cannot do");
            set_connection_info(GLOBAL_STATE, "SV2: Fixed version unsupported");
        }
        return false;
    }

    if (log) {
        ESP_LOGI(TAG, "Pool accepted connection: SV2 version %u, flags 0x%08" PRIx32, used_version, flags);
    }
    return true;
}

static bool open_channel(GlobalState *GLOBAL_STATE, sv2_conn_t *conn, esp_transport_handle_t transport,
                         uint8_t *frame_buf, uint8_t *recv_buf)
{
    const char *user = pool_user(GLOBAL_STATE, conn->pool_id);
    float hash_rate = nominal_hash_rate(GLOBAL_STATE);
    int frame_len;

    ESP_LOGI(TAG, "Opening %s mining channel (user=%s)", sv2_channel_type_to_string(conn->channel_type), user ? user : "");
    if (conn->channel_type == SV2_CHANNEL_EXTENDED) {
        frame_len = sv2_build_open_extended_mining_channel(frame_buf, SV2_MAX_FRAME_SIZE, 1, user,
                                                           hash_rate, SV2_MIN_EXTRANONCE_SIZE);
    } else {
        frame_len = sv2_build_open_standard_mining_channel(frame_buf, SV2_MAX_FRAME_SIZE, 1, user, hash_rate);
    }
    if (frame_len < 0 || sv2_noise_send(conn->noise_ctx, transport, frame_buf, frame_len) != 0) {
        ESP_LOGE(TAG, "Failed to send OpenMiningChannel");
        return false;
    }

    uint8_t hdr_buf[SV2_FRAME_HEADER_SIZE];
    int payload_len = 0;
    if (sv2_noise_recv(conn->noise_ctx, transport, hdr_buf, recv_buf, SV2_MAX_FRAME_SIZE, &payload_len) != 0) {
        ESP_LOGE(TAG, "No reply to OpenMiningChannel");
        return false;
    }

    sv2_frame_header_t hdr;
    sv2_parse_frame_header(hdr_buf, &hdr);
    uint8_t expected = conn->channel_type == SV2_CHANNEL_EXTENDED ? SV2_MSG_OPEN_EXTENDED_MINING_CHANNEL_SUCCESS
                                                                  : SV2_MSG_OPEN_STANDARD_MINING_CHANNEL_SUCCESS;
    if (hdr.msg_type != expected) {
        ESP_LOGE(TAG, "OpenMiningChannel rejected by pool (msg_type=0x%02x)", hdr.msg_type);
        set_connection_info(GLOBAL_STATE, "SV2: Pool rejected miner");
        return false;
    }

    uint32_t request_id;
    uint8_t prefix[SV2_MAX_EXTRANONCE_SIZE];
    uint8_t prefix_len = 0;
    int err;
    if (conn->channel_type == SV2_CHANNEL_EXTENDED) {
        uint16_t extranonce_size;
        err = sv2_parse_open_extended_channel_success(recv_buf, payload_len, &request_id, &conn->channel_id,
                                                      conn->target, &extranonce_size, prefix, &prefix_len,
                                                      &conn->group_channel_id);
        conn->extranonce_size = (uint8_t)extranonce_size;
    } else {
        err = sv2_parse_open_channel_success(recv_buf, payload_len, &request_id, &conn->channel_id,
                                             conn->target, prefix, &prefix_len, &conn->group_channel_id);
    }
    if (err != 0) {
        ESP_LOGE(TAG, "Failed to parse OpenMiningChannel.Success");
        return false;
    }

    if (conn->channel_type == SV2_CHANNEL_EXTENDED) {
        bin2hex(prefix, prefix_len, conn->extranonce_hex, sizeof(conn->extranonce_hex));
        ESP_LOGI(TAG, "Extended channel: extranonce prefix %u bytes, rollable %u bytes", prefix_len, conn->extranonce_size);
    }

    ESP_LOGI(TAG, "Mining channel %" PRIu32 " opened (group %" PRIu32 "), pool difficulty %g",
             conn->channel_id, conn->group_channel_id, target_to_diff(conn->target));
    return true;
}

// Points the shared mining state at this channel: create_jobs_task builds
// extended-channel coinbases from extranonce_str / extranonce_2_len, and the
// BM1370 rolls the BIP320 version bits.
static void apply_channel_to_mining_state(GlobalState *GLOBAL_STATE, sv2_conn_t *conn)
{
    char *extranonce = NULL;
    if (conn->channel_type == SV2_CHANNEL_EXTENDED) {
        extranonce = strdup(conn->extranonce_hex);
    }

    pthread_mutex_lock(&GLOBAL_STATE->stratum_work_lock);
    char *old_extranonce = GLOBAL_STATE->extranonce_str;
    GLOBAL_STATE->extranonce_str = extranonce;
    GLOBAL_STATE->extranonce_2_len = conn->channel_type == SV2_CHANNEL_EXTENDED ? conn->extranonce_size : 0;
    GLOBAL_STATE->version_mask = STRATUM_DEFAULT_VERSION_MASK;
    GLOBAL_STATE->new_stratum_version_rolling_msg = true;
    pthread_mutex_unlock(&GLOBAL_STATE->stratum_work_lock);
    free(old_extranonce);
}

esp_err_t stratum_v2_run(GlobalState *GLOBAL_STATE, int pool_id, bool *got_work)
{
    *got_work = false;

    uint8_t *frame_buf = heap_caps_malloc(SV2_MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM);
    uint8_t *recv_buf = heap_caps_malloc(SV2_MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM);
    if (frame_buf == NULL) frame_buf = malloc(SV2_MAX_FRAME_SIZE);
    if (recv_buf == NULL) recv_buf = malloc(SV2_MAX_FRAME_SIZE);
    sv2_conn_t *conn = calloc(1, sizeof(sv2_conn_t));
    if (frame_buf == NULL || recv_buf == NULL || conn == NULL) {
        ESP_LOGE(TAG, "Out of memory for SV2 session");
        free(frame_buf);
        free(recv_buf);
        free(conn);
        return ESP_ERR_NO_MEM;
    }
    conn->pool_id = pool_id;
    conn->channel_type = pool_channel_type(GLOBAL_STATE, pool_id);

    int64_t connect_start_us = esp_timer_get_time();
    esp_transport_handle_t transport = connect_encrypted(GLOBAL_STATE, pool_id, &conn->noise_ctx, true);
    if (transport == NULL ||
        !setup_connection(GLOBAL_STATE, pool_id, conn->noise_ctx, transport, conn->channel_type,
                          frame_buf, recv_buf, SV2_MAX_FRAME_SIZE, true) ||
        !open_channel(GLOBAL_STATE, conn, transport, frame_buf, recv_buf)) {
        teardown_session(GLOBAL_STATE, transport, conn);
        free(frame_buf);
        free(recv_buf);
        return ESP_FAIL;
    }

    cleanQueue(GLOBAL_STATE);
    apply_channel_to_mining_state(GLOBAL_STATE, conn);
    // Clear the close flag before publishing: a request that lands after this
    // point sets it again and shuts the fd down, so it cannot be lost.
    GLOBAL_STATE->close_requested = false;
    publish_session(GLOBAL_STATE, transport, conn);
    GLOBAL_STATE->abandon_work = 0;

    ESP_LOGI(TAG, "Connected to %s:%u (SV2, encrypted) in %.0f ms, waiting for jobs",
             pool_url(GLOBAL_STATE, pool_id), pool_port(GLOBAL_STATE, pool_id),
             (esp_timer_get_time() - connect_start_us) / 1000.0);

    esp_err_t result;
    uint8_t hdr_buf[SV2_FRAME_HEADER_SIZE];
    while (1) {
        int payload_len = 0;
        // Only this task reads: the noise receive state needs no lock.
        if (sv2_noise_recv(conn->noise_ctx, transport, hdr_buf, recv_buf, SV2_MAX_FRAME_SIZE, &payload_len) != 0) {
            if (GLOBAL_STATE->close_requested) {
                ESP_LOGI(TAG, "Close requested, dropping connection");
                result = ESP_OK;
            } else {
                ESP_LOGE(TAG, "Failed to receive frame, reconnecting");
                result = ESP_FAIL;
            }
            break;
        }

        sv2_frame_header_t hdr;
        sv2_parse_frame_header(hdr_buf, &hdr);
        uint32_t len = (uint32_t)payload_len;

        switch (hdr.msg_type) {
            // Job messages must match the negotiated channel: a standard job
            // has no coinbase, an extended one no merkle root.
            case SV2_MSG_NEW_MINING_JOB:
                if (conn->channel_type != SV2_CHANNEL_STANDARD) {
                    ESP_LOGW(TAG, "Ignoring NewMiningJob on an extended channel");
                } else if (handle_new_mining_job(GLOBAL_STATE, conn, recv_buf, len)) {
                    *got_work = true;
                }
                break;
            case SV2_MSG_NEW_EXTENDED_MINING_JOB:
                if (conn->channel_type != SV2_CHANNEL_EXTENDED) {
                    ESP_LOGW(TAG, "Ignoring NewExtendedMiningJob on a standard channel");
                } else if (handle_new_extended_mining_job(GLOBAL_STATE, conn, recv_buf, len)) {
                    *got_work = true;
                }
                break;
            case SV2_MSG_SET_NEW_PREV_HASH:
                if (handle_set_new_prev_hash(GLOBAL_STATE, conn, recv_buf, len)) {
                    *got_work = true;
                }
                break;
            case SV2_MSG_SET_TARGET:
                handle_set_target(conn, recv_buf, len);
                break;
            case SV2_MSG_SUBMIT_SHARES_SUCCESS:
                handle_submit_shares_success(GLOBAL_STATE, conn, recv_buf, len);
                break;
            case SV2_MSG_SUBMIT_SHARES_ERROR:
                handle_submit_shares_error(GLOBAL_STATE, conn, recv_buf, len);
                break;
            default:
                ESP_LOGW(TAG, "Unhandled SV2 message type 0x%02x (len %" PRIu32 ")", hdr.msg_type, len);
                break;
        }

        if (GLOBAL_STATE->close_requested) {
            ESP_LOGI(TAG, "Close requested, dropping connection");
            result = ESP_OK;
            break;
        }
    }

    teardown_session(GLOBAL_STATE, transport, conn);
    GLOBAL_STATE->close_requested = false;
    cleanQueue(GLOBAL_STATE);
    free(frame_buf);
    free(recv_buf);
    return result;
}

bool stratum_v2_probe_pool(GlobalState *GLOBAL_STATE, int pool_id)
{
    sv2_noise_ctx_t *noise = NULL;
    esp_transport_handle_t transport = connect_encrypted(GLOBAL_STATE, pool_id, &noise, false);
    if (transport == NULL) {
        return false;
    }

    uint8_t *frame_buf = malloc(SV2_PROBE_BUFFER_SIZE);
    uint8_t *recv_buf = malloc(SV2_PROBE_BUFFER_SIZE);
    bool ok = frame_buf != NULL && recv_buf != NULL &&
              setup_connection(GLOBAL_STATE, pool_id, noise, transport, pool_channel_type(GLOBAL_STATE, pool_id),
                               frame_buf, recv_buf, SV2_PROBE_BUFFER_SIZE, false);

    free(frame_buf);
    free(recv_buf);
    sv2_noise_destroy(noise);
    esp_transport_close(transport);
    esp_transport_destroy(transport);
    return ok;
}
