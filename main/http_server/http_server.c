#include <fcntl.h>
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "dns_server.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "lwip/err.h"
#include "lwip/inet.h"
#include "lwip/lwip_napt.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "TPS546.h"
#include "asic.h"
#include "cJSON.h"
#include "connect.h"
#include "global_state.h"
#include "http_server.h"
#include "handler_ota_github.h"
#include "nvs_config.h"
#include "power.h"
#include "system.h"
#include "vcore.h"

static const char * TAG = "http_server";
static const char * CORS_TAG = "CORS";

static bool parse_port(const char *value, uint16_t *port)
{
    unsigned long parsed = 0;

    if (value == NULL || *value == '\0') {
        return false;
    }

    for (const char *p = value; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
        parsed = parsed * 10 + (*p - '0');
        if (parsed > UINT16_MAX) {
            return false;
        }
    }

    *port = (uint16_t)parsed;
    return true;
}

typedef struct {
    bool parsed_port;
    bool parsed_tls;
} stratum_url_parse_result_t;

static stratum_url_parse_result_t save_stratum_url(const char *url_key, const char *port_key, const char *tls_key, const char *value)
{
    stratum_url_parse_result_t result = {0};
    const char *host = value;
    int tls_mode = -1;

    if (strncasecmp(host, "stratum+tcp://", strlen("stratum+tcp://")) == 0) {
        host += strlen("stratum+tcp://");
        tls_mode = DISABLED;
    } else if (strncasecmp(host, "stratum+tls://", strlen("stratum+tls://")) == 0) {
        host += strlen("stratum+tls://");
        tls_mode = BUNDLED_CRT;
    } else if (strncasecmp(host, "stratum+ssl://", strlen("stratum+ssl://")) == 0) {
        host += strlen("stratum+ssl://");
        tls_mode = BUNDLED_CRT;
    }

    char *normalized = strdup(host);
    if (normalized == NULL) {
        return result;
    }

    char *path = strchr(normalized, '/');
    if (path != NULL) {
        *path = '\0';
    }

    char *port_start = strrchr(normalized, ':');
    if (port_start != NULL) {
        uint16_t port;
        if (parse_port(port_start + 1, &port)) {
            *port_start = '\0';
            nvs_config_set_u16(port_key, port);
            result.parsed_port = true;
        }
    }

    nvs_config_set_string(url_key, normalized);
    if (tls_mode >= 0) {
        nvs_config_set_u16(tls_key, (uint16_t)tls_mode);
        result.parsed_tls = true;
    }

    free(normalized);
    return result;
}

/* Handler for WiFi scan endpoint */
static esp_err_t GET_wifi_scan(httpd_req_t * req)
{
    httpd_resp_set_type(req, "application/json");

    // Give some time for the connected flag to take effect
    vTaskDelay(100 / portTICK_PERIOD_MS);

    wifi_ap_record_simple_t ap_records[WIFI_SCAN_RESULT_LIMIT];
    uint16_t ap_count = 0;

    esp_err_t err = wifi_scan(ap_records, &ap_count);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi scan failed");
        return ESP_OK;
    }

    cJSON * root = cJSON_CreateObject();
    cJSON * networks = cJSON_CreateArray();

    for (int i = 0; i < ap_count; i++) {
        cJSON * network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "ssid", (char *) ap_records[i].ssid);
        cJSON_AddNumberToObject(network, "rssi", ap_records[i].rssi);
        cJSON_AddNumberToObject(network, "authmode", ap_records[i].authmode);
        cJSON_AddItemToArray(networks, network);
    }

    cJSON_AddItemToObject(root, "networks", networks);

    const char * response = cJSON_Print(root);
    cJSON_Delete(root);
    if (response == NULL) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    httpd_resp_sendstr(req, response);
    free((void *) response);
    return ESP_OK;
}

static GlobalState * GLOBAL_STATE;
static httpd_handle_t server = NULL;

// Check if an HTTP request arrived on the AP network interface
// by comparing the socket's local address against the AP interface IP
static bool is_request_from_ap(httpd_req_t * req)
{
    esp_netif_t * ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == NULL) {
        return false;
    }

    esp_netif_ip_info_t ap_ip_info;
    if (esp_netif_get_ip_info(ap_netif, &ap_ip_info) != ESP_OK) {
        return false;
    }

    int sockfd = httpd_req_to_sockfd(req);
    struct sockaddr_in6 local_addr;
    socklen_t addr_size = sizeof(local_addr);

    // Use getsockname to get the LOCAL address (which interface the request arrived on)
    if (getsockname(sockfd, (struct sockaddr *) &local_addr, &addr_size) != 0) {
        return false;
    }

    uint32_t local_ip = local_addr.sin6_addr.un.u32_addr[3];
    // If the socket's local IP matches the AP interface IP, the request came via the AP
    return local_ip == ap_ip_info.ip.addr;
}

QueueHandle_t log_queue = NULL;

static atomic_int websocket_fd = ATOMIC_VAR_INIT(-1);

typedef struct {
    int fd;
    char text[];
} websocket_log_message_t;

#define REST_CHECK(a, str, goto_tag, ...)                                                                                          \
    do {                                                                                                                           \
        if (!(a)) {                                                                                                                \
            ESP_LOGE(TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__);                                                  \
            goto goto_tag;                                                                                                         \
        }                                                                                                                          \
    } while (0)

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)
#define SCRATCH_BUFSIZE (10240)
#define MESSAGE_QUEUE_SIZE (128)

typedef struct rest_server_context
{
    char base_path[ESP_VFS_PATH_MAX + 1];
    char scratch[SCRATCH_BUFSIZE];
} rest_server_context_t;

#define CHECK_FILE_EXTENSION(filename, ext) (strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

static esp_err_t ip_in_private_range(uint32_t address)
{
    uint32_t ip_address = ntohl(address);

    // 10.0.0.0 - 10.255.255.255 (Class A)
    if ((ip_address >= 0x0A000000) && (ip_address <= 0x0AFFFFFF)) {
        return ESP_OK;
    }

    // 172.16.0.0 - 172.31.255.255 (Class B)
    if ((ip_address >= 0xAC100000) && (ip_address <= 0xAC1FFFFF)) {
        return ESP_OK;
    }

    // 192.168.0.0 - 192.168.255.255 (Class C)
    if ((ip_address >= 0xC0A80000) && (ip_address <= 0xC0A8FFFF)) {
        return ESP_OK;
    }

    return ESP_FAIL;
}

static uint32_t extract_origin_ip_addr(char * origin)
{
    char ip_str[16];
    uint32_t origin_ip_addr = 0;

    // Find the start of the IP address in the Origin header
    const char * prefix = "http://";
    char * ip_start = strstr(origin, prefix);
    if (ip_start) {
        ip_start += strlen(prefix); // Move past "http://"

        // Extract the IP address portion (up to the next '/')
        char * ip_end = strchr(ip_start, '/');
        size_t ip_len = ip_end ? (size_t) (ip_end - ip_start) : strlen(ip_start);
        if (ip_len < sizeof(ip_str)) {
            strncpy(ip_str, ip_start, ip_len);
            ip_str[ip_len] = '\0'; // Null-terminate the string

            // Convert the IP address string to uint32_t
            origin_ip_addr = inet_addr(ip_str);
            if (origin_ip_addr == INADDR_NONE) {
                ESP_LOGW(CORS_TAG, "Invalid IP address: %s", ip_str);
            } else {
                ESP_LOGD(CORS_TAG, "Extracted IP address %lu", origin_ip_addr);
            }
        } else {
            ESP_LOGW(CORS_TAG, "IP address string is too long: %s", ip_start);
        }
    }

    return origin_ip_addr;
}

esp_err_t is_network_allowed(httpd_req_t * req)
{
    if (GLOBAL_STATE->SYSTEM_MODULE.ap_enabled == true) {
        ESP_LOGD(CORS_TAG, "Device in AP mode. Allowing CORS.");
        return ESP_OK;
    }

    int sockfd = httpd_req_to_sockfd(req);
    char ipstr[INET6_ADDRSTRLEN];
    struct sockaddr_in6 addr; // esp_http_server uses IPv6 addressing
    socklen_t addr_size = sizeof(addr);

    if (getpeername(sockfd, (struct sockaddr *) &addr, &addr_size) < 0) {
        ESP_LOGE(CORS_TAG, "Error getting client IP");
        return ESP_FAIL;
    }

    uint32_t request_ip_addr = addr.sin6_addr.un.u32_addr[3];

    // // Convert to IPv6 string
    // inet_ntop(AF_INET, &addr.sin6_addr, ipstr, sizeof(ipstr));

    // Convert to IPv4 string
    inet_ntop(AF_INET, &request_ip_addr, ipstr, sizeof(ipstr));

    // Attempt to get the Origin header.
    char origin[128];
    uint32_t origin_ip_addr;
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) == ESP_OK) {
        ESP_LOGD(CORS_TAG, "Origin header: %s", origin);
        origin_ip_addr = extract_origin_ip_addr(origin);
    } else {
        ESP_LOGD(CORS_TAG, "No origin header found.");
        origin_ip_addr = request_ip_addr;
    }

    if (ip_in_private_range(origin_ip_addr) == ESP_OK && ip_in_private_range(request_ip_addr) == ESP_OK) {
        return ESP_OK;
    }

    ESP_LOGW(CORS_TAG, "Client is NOT in the private ip ranges or same range as server.");
    return ESP_FAIL;
}

esp_err_t init_fs(void)
{
    esp_vfs_spiffs_conf_t conf = {.base_path = "", .partition_label = NULL, .max_files = 5, .format_if_mount_failed = false};
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
    return ESP_OK;
}

/* Function for stopping the webserver */
void stop_webserver(httpd_handle_t server)
{
    if (server) {
        /* Stop the httpd server */
        httpd_stop(server);
    }
}

/* Set HTTP response content type according to file extension */
static esp_err_t set_content_type_from_file(httpd_req_t * req, const char * filepath)
{
    const char * type = "text/plain";
    if (CHECK_FILE_EXTENSION(filepath, ".html")) {
        type = "text/html";
    } else if (CHECK_FILE_EXTENSION(filepath, ".js")) {
        type = "application/javascript";
    } else if (CHECK_FILE_EXTENSION(filepath, ".css")) {
        type = "text/css";
    } else if (CHECK_FILE_EXTENSION(filepath, ".png")) {
        type = "image/png";
    } else if (CHECK_FILE_EXTENSION(filepath, ".ico")) {
        type = "image/x-icon";
    } else if (CHECK_FILE_EXTENSION(filepath, ".svg")) {
        type = "text/xml";
    } else if (CHECK_FILE_EXTENSION(filepath, ".pdf")) {
        type = "application/pdf";
    }
    return httpd_resp_set_type(req, type);
}

esp_err_t set_cors_headers(httpd_req_t * req)
{

    esp_err_t err;

    err = httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (err != ESP_OK) {
        return ESP_FAIL;
    }

    err = httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
    if (err != ESP_OK) {
        return ESP_FAIL;
    }

    err = httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    if (err != ESP_OK) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* Handler for AP info endpoint */
static esp_err_t GET_ap_info(httpd_req_t * req)
{
    httpd_resp_set_type(req, "application/json");

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    cJSON * root = cJSON_CreateObject();

    // Check if STA is connected by checking if IP address is set
    bool sta_connected =
        (strlen(GLOBAL_STATE->SYSTEM_MODULE.ip_addr_str) > 0 && strcmp(GLOBAL_STATE->SYSTEM_MODULE.ip_addr_str, "0.0.0.0") != 0);

    cJSON_AddBoolToObject(root, "staConnected", sta_connected);
    cJSON_AddStringToObject(root, "staIp", GLOBAL_STATE->SYSTEM_MODULE.ip_addr_str);
    cJSON_AddStringToObject(root, "apSsid", GLOBAL_STATE->SYSTEM_MODULE.ap_ssid);
    cJSON_AddStringToObject(root, "wifiStatus", GLOBAL_STATE->SYSTEM_MODULE.wifi_status);

    const char * response = cJSON_Print(root);
    cJSON_Delete(root);
    if (response == NULL) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    httpd_resp_sendstr(req, response);
    free((void *) response);
    return ESP_OK;
}

/* Handler for AP status page - only accessible from AP network */
static esp_err_t rest_ap_page_handler(httpd_req_t * req)
{
    if (!GLOBAL_STATE->SYSTEM_MODULE.ap_enabled || !is_request_from_ap(req)) {
        // Not from AP network - redirect to main web interface
        httpd_resp_set_status(req, "302 Temporary Redirect");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, "Redirecting", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    extern const unsigned char ap_page_start[] asm("_binary_ap_page_html_start");
    extern const unsigned char ap_page_end[] asm("_binary_ap_page_html_end");
    const size_t ap_page_size = (ap_page_end - ap_page_start);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *) ap_page_start, ap_page_size);
    return ESP_OK;
}

/* Recovery handler */
static esp_err_t rest_recovery_handler(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    extern const unsigned char recovery_page_start[] asm("_binary_recovery_page_html_start");
    extern const unsigned char recovery_page_end[] asm("_binary_recovery_page_html_end");
    const size_t recovery_page_size = (recovery_page_end - recovery_page_start);
    httpd_resp_send_chunk(req, (const char *) recovery_page_start, recovery_page_size);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Send a 404 as JSON for unhandled api routes */
static esp_err_t rest_api_common_handler(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    cJSON * root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", "Not Found");
    cJSON_AddStringToObject(root, "message", "The requested API endpoint does not exist");

    const char * response = cJSON_Print(root);
    cJSON_Delete(root);
    if (response == NULL) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    httpd_resp_sendstr(req, response);
    free((void *) response);

    return ESP_OK;
}

/* Send HTTP response with the contents of the requested file */
static esp_err_t rest_common_get_handler(httpd_req_t * req)
{
    char filepath[FILE_PATH_MAX];
    size_t filePathLength = sizeof(filepath);

    rest_server_context_t * rest_context = (rest_server_context_t *) req->user_ctx;
    strlcpy(filepath, rest_context->base_path, filePathLength);
    if (req->uri[strlen(req->uri) - 1] == '/') {
        strlcat(filepath, "/index.html", filePathLength);
    } else {
        strlcat(filepath, req->uri, filePathLength);
    }
    set_content_type_from_file(req, filepath);
    strlcat(filepath, ".gz", sizeof(filepath));
    int fd = open(filepath, O_RDONLY, 0);
    if (fd == -1) {
        // Set status
        httpd_resp_set_status(req, "302 Temporary Redirect");
        // Redirect to the "/" root directory
        httpd_resp_set_hdr(req, "Location", "/");
        // iOS requires content in the response to detect a captive portal, simply redirecting is not sufficient.
        httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);

        ESP_LOGI(TAG, "Redirecting to root");
        return ESP_OK;
    }

    if (req->uri[strlen(req->uri) - 1] != '/') {
        httpd_resp_set_hdr(req, "Cache-Control", "max-age=2592000");
    }

    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

    char * chunk = rest_context->scratch;
    ssize_t read_bytes;
    do {
        /* Read file in chunks into the scratch buffer */
        read_bytes = read(fd, chunk, SCRATCH_BUFSIZE);
        if (read_bytes == -1) {
            ESP_LOGE(TAG, "Failed to read file : %s", filepath);
        } else if (read_bytes > 0) {
            /* Send the buffer contents as HTTP response chunk */
            if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
                close(fd);
                ESP_LOGE(TAG, "File sending failed!");
                /* Abort sending file */
                httpd_resp_sendstr_chunk(req, NULL);
                /* Respond with 500 Internal Server Error */
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
                return ESP_OK;
            }
        }
    } while (read_bytes > 0);
    /* Close file after sending complete */
    close(fd);
    ESP_LOGI(TAG, "File sending complete");
    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_options_request(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    // Set CORS headers for OPTIONS request
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    // Send a blank response for OPTIONS request
    httpd_resp_send(req, NULL, 0);

    return ESP_OK;
}

static esp_err_t PATCH_update_settings(httpd_req_t * req)
{

    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    int total_len = req->content_len;
    int cur_len = 0;
    char * buf = ((rest_server_context_t *) (req->user_ctx))->scratch;
    int received = 0;
    if (total_len >= SCRATCH_BUFSIZE) {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
        return ESP_OK;
    }
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post control value");
            return ESP_OK;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON * root = cJSON_Parse(buf);
    cJSON * item;
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_OK;
    }

    stratum_url_parse_result_t stratum_url_result = {0};
    stratum_url_parse_result_t fallback_stratum_url_result = {0};

    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "stratumURL"))) {
        stratum_url_result = save_stratum_url(NVS_CONFIG_STRATUM_URL, NVS_CONFIG_STRATUM_PORT, NVS_CONFIG_STRATUM_TLS, item->valuestring);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "fallbackStratumURL"))) {
        fallback_stratum_url_result = save_stratum_url(NVS_CONFIG_FALLBACK_STRATUM_URL, NVS_CONFIG_FALLBACK_STRATUM_PORT, NVS_CONFIG_FALLBACK_STRATUM_TLS, item->valuestring);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "stratumUser"))) {
        nvs_config_set_string(NVS_CONFIG_STRATUM_USER, item->valuestring);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "stratumPassword"))) {
        nvs_config_set_string(NVS_CONFIG_STRATUM_PASS, item->valuestring);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "fallbackStratumUser"))) {
        nvs_config_set_string(NVS_CONFIG_FALLBACK_STRATUM_USER, item->valuestring);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "fallbackStratumPassword"))) {
        nvs_config_set_string(NVS_CONFIG_FALLBACK_STRATUM_PASS, item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(root, "stratumPort")) != NULL && !stratum_url_result.parsed_port) {
        nvs_config_set_u16(NVS_CONFIG_STRATUM_PORT, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "fallbackStratumPort")) != NULL && !fallback_stratum_url_result.parsed_port) {
        nvs_config_set_u16(NVS_CONFIG_FALLBACK_STRATUM_PORT, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "stratumTLS")) != NULL && !stratum_url_result.parsed_tls) {
        nvs_config_set_u16(NVS_CONFIG_STRATUM_TLS, item->valueint);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "stratumCert"))) {
        nvs_config_set_string(NVS_CONFIG_STRATUM_CERT, item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(root, "fallbackStratumTLS")) != NULL && !fallback_stratum_url_result.parsed_tls) {
        nvs_config_set_u16(NVS_CONFIG_FALLBACK_STRATUM_TLS, item->valueint);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "fallbackStratumCert"))) {
        nvs_config_set_string(NVS_CONFIG_FALLBACK_STRATUM_CERT, item->valuestring);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "ssid"))) {
        nvs_config_set_string(NVS_CONFIG_WIFI_SSID, item->valuestring);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "wifiPass"))) {
        nvs_config_set_string(NVS_CONFIG_WIFI_PASS, item->valuestring);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "hostname"))) {
        nvs_config_set_string(NVS_CONFIG_HOSTNAME, item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(root, "coreVoltage")) != NULL && item->valueint > 0) {
        nvs_config_set_u16(NVS_CONFIG_ASIC_VOLTAGE, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "frequency")) != NULL && item->valueint > 0) {
        nvs_config_set_u16(NVS_CONFIG_ASIC_FREQ, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "overheat_mode")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_OVERHEAT_MODE, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "autofanspeed")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_AUTO_FAN_SPEED, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "fanSpeed")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_FAN_SPEED, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "manualFanSpeed")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_FAN_SPEED, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "overclockEnabled")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_OVERCLOCK_ENABLED, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "poolMode")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_POOL_MODE, item->valueint ? POOL_MODE_DUAL : POOL_MODE_FALLBACK);
    }
    if ((item = cJSON_GetObjectItem(root, "poolBalance")) != NULL) {
        int balance = item->valueint;
        if (balance < 0) {
            balance = 0;
        } else if (balance > 100) {
            balance = 100;
        }
        nvs_config_set_u16(NVS_CONFIG_POOL_BALANCE, balance);
    }

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true}");
    return ESP_OK;
}

/* Report whether the share LED is allowed to blink */
static esp_err_t GET_system_led(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    httpd_resp_set_type(req, "application/json");

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    cJSON * root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ledBlinkEnabled", SYSTEM_get_led_blink_enabled(GLOBAL_STATE));

    const char * response = cJSON_Print(root);
    cJSON_Delete(root);
    if (response == NULL) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    httpd_resp_sendstr(req, response);
    free((char *) response);
    return ESP_OK;
}

/* Enable or disable the accepted-share LED blink */
static esp_err_t PATCH_system_led(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    int total_len = req->content_len;
    int cur_len = 0;
    char * buf = ((rest_server_context_t *) (req->user_ctx))->scratch;
    int received = 0;
    if (total_len >= SCRATCH_BUFSIZE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
        return ESP_OK;
    }
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post control value");
            return ESP_OK;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON * root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_OK;
    }

    // Accept both true/false and 1/0
    cJSON * item = cJSON_GetObjectItem(root, "ledBlinkEnabled");
    if (!cJSON_IsBool(item) && !cJSON_IsNumber(item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid ledBlinkEnabled");
        return ESP_OK;
    }

    bool enabled = cJSON_IsBool(item) ? cJSON_IsTrue(item) : (item->valueint != 0);
    cJSON_Delete(root);

    SYSTEM_set_led_blink_enabled(GLOBAL_STATE, enabled);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, enabled ? "{\"success\":true,\"ledBlinkEnabled\":true}"
                                    : "{\"success\":true,\"ledBlinkEnabled\":false}");
    return ESP_OK;
}

static esp_err_t POST_restart(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Restarting System because of API Request");

    // Send HTTP response before restarting
    const char * resp_str = "System will restart shortly.";
    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);

    // Delay to ensure the response is sent
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    // Restart the system
    esp_restart();

    // This return statement will never be reached, but it's good practice to include it
    return ESP_OK;
}

/* Simple handler for getting system handler */
static esp_err_t GET_system_info(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    httpd_resp_set_type(req, "application/json");

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    // Detect if request is coming from AP network using actual AP netif subnet
    bool request_from_ap = is_request_from_ap(req);

    char * ssid = nvs_config_get_string(NVS_CONFIG_WIFI_SSID, CONFIG_ESP_WIFI_SSID);
    char * hostname = nvs_config_get_string(NVS_CONFIG_HOSTNAME, CONFIG_LWIP_LOCAL_HOSTNAME);
    uint8_t mac[6];
    char formattedMac[18];
    char * stratumURL = nvs_config_get_string(NVS_CONFIG_STRATUM_URL, CONFIG_STRATUM_URL);
    char * fallbackStratumURL = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_URL, CONFIG_FALLBACK_STRATUM_URL);
    char * stratumUser = nvs_config_get_string(NVS_CONFIG_STRATUM_USER, CONFIG_STRATUM_USER);
    char * fallbackStratumUser = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_USER, CONFIG_FALLBACK_STRATUM_USER);
    char * board_version = nvs_config_get_string(NVS_CONFIG_BOARD_VERSION, "unknown");

    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(formattedMac, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    int8_t wifi_rssi = -90;
    get_wifi_current_rssi(&wifi_rssi);

    cJSON * root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "power", GLOBAL_STATE->POWER_MANAGEMENT_MODULE.power);
    cJSON_AddNumberToObject(root, "voltage", GLOBAL_STATE->POWER_MANAGEMENT_MODULE.voltage);
    cJSON_AddNumberToObject(root, "current", Power_get_current(GLOBAL_STATE));
    cJSON_AddNumberToObject(root, "temp", GLOBAL_STATE->POWER_MANAGEMENT_MODULE.chip_temp_avg);
    cJSON_AddNumberToObject(root, "vrTemp", GLOBAL_STATE->POWER_MANAGEMENT_MODULE.vr_temp);
    cJSON_AddNumberToObject(root, "maxPower", Power_get_max_settings(GLOBAL_STATE));
    cJSON_AddNumberToObject(root, "nominalVoltage", Power_get_nominal_voltage(GLOBAL_STATE));
    cJSON_AddNumberToObject(root, "hashRate", GLOBAL_STATE->HASHRATE_MONITOR_MODULE.hashrate);
    cJSON_AddNumberToObject(root, "bestDiff", GLOBAL_STATE->SYSTEM_MODULE.best_nonce_diff);
    cJSON_AddStringToObject(root, "bestSessionDiff", GLOBAL_STATE->SYSTEM_MODULE.best_session_diff_string);
    cJSON_AddNumberToObject(root, "stratumDiff", GLOBAL_STATE->stratum_difficulty);

    cJSON_AddNumberToObject(root, "isUsingFallbackStratum", GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback);

    cJSON_AddNumberToObject(root, "isPSRAMAvailable", GLOBAL_STATE->psram_is_available);

    cJSON_AddNumberToObject(root, "freeHeap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "freeHeapInternal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "freeHeapSpiram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "coreVoltage", nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE, CONFIG_ASIC_VOLTAGE));
    cJSON_AddNumberToObject(root, "coreVoltageActual", VCORE_get_voltage_mv(GLOBAL_STATE));
    cJSON_AddNumberToObject(root, "frequency", nvs_config_get_u16(NVS_CONFIG_ASIC_FREQ, CONFIG_ASIC_FREQUENCY));
    cJSON_AddStringToObject(root, "ssid", ssid);
    cJSON_AddStringToObject(root, "macAddr", formattedMac);
    cJSON_AddStringToObject(root, "hostname", hostname);
    cJSON_AddStringToObject(root, "wifiStatus", GLOBAL_STATE->SYSTEM_MODULE.wifi_status);
    cJSON_AddNumberToObject(root, "wifiRSSI", wifi_rssi);
    cJSON_AddNumberToObject(root, "apEnabled", GLOBAL_STATE->SYSTEM_MODULE.ap_enabled);
    cJSON_AddBoolToObject(root, "requestFromAp", request_from_ap);
    cJSON_AddNumberToObject(root, "sharesAccepted", GLOBAL_STATE->SYSTEM_MODULE.shares_accepted);
    cJSON_AddNumberToObject(root, "sharesRejected", GLOBAL_STATE->SYSTEM_MODULE.shares_rejected);
    cJSON_AddNumberToObject(root, "poolMode", nvs_config_get_u16(NVS_CONFIG_POOL_MODE, POOL_MODE_FALLBACK));
    cJSON_AddNumberToObject(root, "poolBalance", nvs_config_get_u16(NVS_CONFIG_POOL_BALANCE, 50));

    cJSON * pools_array = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "pools", pools_array);
    for (int i = 0; i < POOL_COUNT; i++) {
        cJSON * pool_obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(pool_obj, "connected", GLOBAL_STATE->pools[i].connected);
        cJSON_AddBoolToObject(pool_obj, "validNotify", GLOBAL_STATE->pools[i].valid_notify);
        cJSON_AddNumberToObject(pool_obj, "difficulty", GLOBAL_STATE->pools[i].stratum_difficulty);
        cJSON_AddNumberToObject(pool_obj, "accepted", GLOBAL_STATE->pools[i].shares_accepted);
        cJSON_AddNumberToObject(pool_obj, "rejected", GLOBAL_STATE->pools[i].shares_rejected);
        cJSON_AddItemToArray(pools_array, pool_obj);
    }

    cJSON * error_array = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "sharesRejectedReasons", error_array);

    for (int i = 0; i < GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats_count; i++) {
        cJSON * error_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(error_obj, "message", GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats[i].message);
        cJSON_AddNumberToObject(error_obj, "count", GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats[i].count);
        cJSON_AddItemToArray(error_array, error_obj);
    }

    cJSON_AddNumberToObject(root, "uptimeSeconds", (esp_timer_get_time() - GLOBAL_STATE->SYSTEM_MODULE.start_time) / 1000000);
    cJSON_AddNumberToObject(root, "asicCount", ASIC_get_asic_count(GLOBAL_STATE));
    cJSON_AddNumberToObject(root, "smallCoreCount", ASIC_get_small_core_count(GLOBAL_STATE));
    cJSON_AddStringToObject(root, "ASICModel", GLOBAL_STATE->asic_model_str);
    cJSON_AddStringToObject(root, "stratumURL", stratumURL);
    cJSON_AddStringToObject(root, "fallbackStratumURL", fallbackStratumURL);
    cJSON_AddNumberToObject(root, "stratumPort", nvs_config_get_u16(NVS_CONFIG_STRATUM_PORT, CONFIG_STRATUM_PORT));
    cJSON_AddNumberToObject(root, "fallbackStratumPort",
                            nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_PORT, CONFIG_FALLBACK_STRATUM_PORT));
    cJSON_AddStringToObject(root, "stratumUser", stratumUser);
    cJSON_AddStringToObject(root, "fallbackStratumUser", fallbackStratumUser);
    cJSON_AddNumberToObject(root, "stratumTLS", nvs_config_get_u16(NVS_CONFIG_STRATUM_TLS, CONFIG_STRATUM_TLS));
    cJSON_AddNumberToObject(root, "fallbackStratumTLS",
                            nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_TLS, CONFIG_FALLBACK_STRATUM_TLS));

    char * stratumCert = nvs_config_get_string(NVS_CONFIG_STRATUM_CERT, CONFIG_STRATUM_CERT);
    char * fallbackStratumCert = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_CERT, CONFIG_FALLBACK_STRATUM_CERT);
    cJSON_AddStringToObject(root, "stratumCert", stratumCert);
    cJSON_AddStringToObject(root, "fallbackStratumCert", fallbackStratumCert);

    cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "idfVersion", esp_get_idf_version());
    cJSON_AddStringToObject(root, "boardVersion", board_version);
    cJSON_AddStringToObject(root, "runningPartition", esp_ota_get_running_partition()->label);

    cJSON_AddNumberToObject(root, "overheat_mode", nvs_config_get_u16(NVS_CONFIG_OVERHEAT_MODE, 0));
    cJSON_AddNumberToObject(root, "overclockEnabled", nvs_config_get_u16(NVS_CONFIG_OVERCLOCK_ENABLED, 0));
    cJSON_AddBoolToObject(root, "ledBlinkEnabled", SYSTEM_get_led_blink_enabled(GLOBAL_STATE));

    cJSON_AddNumberToObject(root, "autofanspeed", nvs_config_get_u16(NVS_CONFIG_AUTO_FAN_SPEED, 1));

    cJSON_AddNumberToObject(root, "fanSpeed", GLOBAL_STATE->POWER_MANAGEMENT_MODULE.fan_perc);
    cJSON_AddNumberToObject(root, "fanrpm", GLOBAL_STATE->POWER_MANAGEMENT_MODULE.fan_rpm[0]);
    cJSON_AddNumberToObject(root, "manualFanSpeed", nvs_config_get_u16(NVS_CONFIG_FAN_SPEED, 100));

    cJSON_AddNumberToObject(root, "chiptemp1", GLOBAL_STATE->POWER_MANAGEMENT_MODULE.chip_temp[0]);
    cJSON_AddNumberToObject(root, "chiptemp2", GLOBAL_STATE->POWER_MANAGEMENT_MODULE.chip_temp[1]);

    if (GLOBAL_STATE->SYSTEM_MODULE.power_fault > 0) {
        cJSON_AddStringToObject(root, "power_fault", VCORE_get_fault_string(GLOBAL_STATE));
    }

    free(ssid);
    free(hostname);
    free(stratumURL);
    free(fallbackStratumURL);
    free(stratumUser);
    free(fallbackStratumUser);
    free(stratumCert);
    free(fallbackStratumCert);
    free(board_version);

    const char * sys_info = cJSON_Print(root);
    cJSON_Delete(root);
    if (sys_info == NULL) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    httpd_resp_sendstr(req, sys_info);
    free((char *) sys_info);
    return ESP_OK;
}

/* Handler for system ASIC info endpoint */
static esp_err_t GET_system_asic(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    httpd_resp_set_type(req, "application/json");

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    cJSON * root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "ASICModel", GLOBAL_STATE->asic_model_str);
    cJSON_AddStringToObject(root, "deviceModel", GLOBAL_STATE->device_model_str);
    cJSON_AddNumberToObject(root, "asicCount", ASIC_get_asic_count(GLOBAL_STATE));
    cJSON_AddNumberToObject(root, "smallCoreCount", ASIC_get_small_core_count(GLOBAL_STATE));
    cJSON_AddNumberToObject(root, "defaultFrequency", CONFIG_ASIC_FREQUENCY);
    cJSON_AddNumberToObject(root, "defaultVoltage", CONFIG_ASIC_VOLTAGE);

    const char * response = cJSON_Print(root);
    cJSON_Delete(root);
    if (response == NULL) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    httpd_resp_sendstr(req, response);
    free((void *) response);
    return ESP_OK;
}

esp_err_t POST_WWW_update(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = true;
    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_filename, 20, "www.bin");
    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Starting...");

    char buf[1000];
    int remaining = req->content_len;

    const esp_partition_t * www_partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "www");
    if (www_partition == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WWW partition not found");
        return ESP_OK;
    }

    // Don't attempt to write more than what can be stored in the partition
    if (remaining > www_partition->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File provided is too large for device");
        return ESP_OK;
    }

    // Erase the entire www partition before writing, in chunks to prevent WDT timeout
    size_t erase_size = 65536; // 64KB chunks
    for (size_t offset = 0; offset < www_partition->size; offset += erase_size) {
        size_t size_to_erase = MIN(erase_size, www_partition->size - offset);
        if (esp_partition_erase_range(www_partition, offset, size_to_erase) != ESP_OK) {
            GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Erase Error");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Partition erase failed");
            return ESP_OK;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    int chunks = 0;
    int timeout_retries = 0;
    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));

        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeout_retries > 30) {
                snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Timeout");
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload timed out");
                GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
                return ESP_OK;
            }
            continue;
        }
        timeout_retries = 0;

        if (recv_len <= 0) {
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Protocol Error");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Protocol Error");
            GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
            return ESP_OK;
        }

        int offset = req->content_len - remaining;
        if (esp_partition_write(www_partition, offset, (const void *) buf, recv_len) != ESP_OK) {
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Write Error");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write Error");
            GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
            return ESP_OK;
        }

        uint8_t percentage = 100 - ((remaining * 100 / req->content_len));
        snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Working (%d%%)", percentage);

        remaining -= recv_len;

        chunks++;
        if (chunks % 16 == 0) {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }

    httpd_resp_sendstr(req, "WWW update complete\n");

snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Finished...");
vTaskDelay(1000 / portTICK_PERIOD_MS);
GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;

return ESP_OK;
}

/*
 * Handle OTA file upload
 */
esp_err_t POST_OTA_update(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not allowed in AP-only mode");
        return ESP_OK;
    }

    GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = true;
    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_filename, 20, "bitforgeos.bin");
    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Starting...");

    char buf[1000];
    esp_ota_handle_t ota_handle;
    int remaining = req->content_len;

    const esp_partition_t * ota_partition = esp_ota_get_next_update_partition(NULL);
    if (ota_partition == NULL) {
        GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
        snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "No OTA partition");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition found");
        return ESP_OK;
    }
    if (esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle) != ESP_OK) {
        GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
        snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "OTA Begin Error");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_OK;
    }

    int chunks = 0;
    int timeout_retries = 0;
    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));

        // Timeout Error: Retry up to 30 times before aborting
        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeout_retries > 30) {
                esp_ota_abort(ota_handle);
                GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
                snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Timeout");
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload timed out");
                return ESP_OK;
            }
            continue;

            // Serious Error: Abort OTA
        } else if (recv_len <= 0) {
            esp_ota_abort(ota_handle);
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Protocol Error");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Protocol Error");
            GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
            return ESP_OK;
        }
        timeout_retries = 0;

        // Successful Upload: Flash firmware chunk
        if (esp_ota_write(ota_handle, (const void *) buf, recv_len) != ESP_OK) {
            esp_ota_abort(ota_handle);
            GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Write Error");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write Error");
            return ESP_OK;
        }

        uint8_t percentage = 100 - ((remaining * 100 / req->content_len));

        snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Working (%d%%)", percentage);

        remaining -= recv_len;

        chunks++;
        if (chunks % 16 == 0) {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }

    // Validate and switch to new OTA image and reboot
    if (esp_ota_end(ota_handle) != ESP_OK || esp_ota_set_boot_partition(ota_partition) != ESP_OK) {
        snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Validation Error");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Validation / Activation Error");
        GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
        return ESP_OK;
    }

    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Rebooting...");

    httpd_resp_sendstr(req, "Firmware update complete, rebooting now!\n");
    ESP_LOGI(TAG, "Restarting System because of Firmware update complete");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_restart();

    return ESP_OK;
}

int log_to_queue(const char * format, va_list args)
{
    // calloc/printf/xQueueSendToBack are not ISR-safe — fall back to vprintf from ISR context
    if (xPortInIsrContext()) {
        return vprintf(format, args);
    }

    va_list args_copy;
    va_copy(args_copy, args);

    // Calculate the required buffer size
    int formatted_len = vsnprintf(NULL, 0, format, args_copy);
    va_end(args_copy);
    if (formatted_len < 0) {
        return 0;
    }

    websocket_log_message_t * message = calloc(1, sizeof(*message) + formatted_len + 2);
    if (message == NULL) {
        return 0;
    }

    // Format the string into the allocated buffer
    va_copy(args_copy, args);
    vsnprintf(message->text, formatted_len + 1, format, args_copy);
    va_end(args_copy);

    // Ensure the log message ends with a newline
    size_t len = strlen(message->text);
    if (len > 0 && message->text[len - 1] != '\n') {
        message->text[len] = '\n';
        message->text[len + 1] = '\0';
        len++;
    }

    // Print to standard output
    printf("%s", message->text);

    if (log_queue == NULL || xQueueSendToBack(log_queue, &message, (TickType_t) 0) != pdPASS) {
        free(message);
    }

    return 0;
}

static void send_log_to_websocket(void * arg)
{
    websocket_log_message_t * message = arg;

    // Prepare the WebSocket frame
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t *) message->text;
    ws_pkt.len = strlen(message->text);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    // This callback runs in the HTTPD task, so the descriptor cannot be
    // closed and reused between validation and transmission.
    if (server != NULL && message->fd == atomic_load(&websocket_fd) &&
        httpd_ws_get_fd_info(server, message->fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
        if (httpd_ws_send_frame_async(server, message->fd, &ws_pkt) == ESP_OK) {
            httpd_sess_update_lru_counter(server, message->fd);
        } else {
            int expected_fd = message->fd;
            if (atomic_compare_exchange_strong(&websocket_fd, &expected_fd, -1)) {
                esp_log_set_vprintf(vprintf);
            }
            httpd_sess_trigger_close(server, message->fd);
        }
    }

    free(message);
}

static void http_session_close(httpd_handle_t handle, int sockfd)
{
    int expected_fd = sockfd;
    if (atomic_compare_exchange_strong(&websocket_fd, &expected_fd, -1)) {
        esp_log_set_vprintf(vprintf);
    }

    (void) handle;
    close(sockfd);
}

/*
 * This handler echos back the received ws data
 * and triggers an async send if certain message received
 */
esp_err_t echo_handler(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        int new_fd = httpd_req_to_sockfd(req);
        int old_fd = atomic_exchange(&websocket_fd, new_fd);
        esp_log_set_vprintf(log_to_queue);

        // Logging supports one client. Close the previous WebSocket instead
        // of leaving an unused persistent session in the HTTPD socket table.
        if (old_fd >= 0 && old_fd != new_fd &&
            httpd_ws_get_fd_info(server, old_fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
            httpd_sess_trigger_close(server, old_fd);
        }
        return ESP_OK;
    }

    // The logging WebSocket is server-to-client only. Read the frame header,
    // then close clients that send application data.
    httpd_ws_frame_t ws_pkt = {0};
    if (httpd_ws_recv_frame(req, &ws_pkt, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    return ws_pkt.len == 0 ? ESP_OK : ESP_FAIL;
}

// HTTP Error (404) Handler - Redirects all requests to the root page
esp_err_t http_404_error_handler(httpd_req_t * req, httpd_err_code_t err)
{
    // Set status
    httpd_resp_set_status(req, "302 Temporary Redirect");
    // Redirect to the "/" root directory
    httpd_resp_set_hdr(req, "Location", "/");
    // iOS requires content in the response to detect a captive portal, simply redirecting is not sufficient.
    httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "Redirecting to root");
    return ESP_OK;
}

void websocket_log_handler()
{
    while (true) {
        websocket_log_message_t * message = NULL;
        if (xQueueReceive(log_queue, &message, (TickType_t) portMAX_DELAY) != pdPASS) {
            // message was never written by xQueueReceive — do not access it
            vTaskDelay(10 / portTICK_PERIOD_MS);
            continue;
        }

        message->fd = atomic_load(&websocket_fd);
        if (server == NULL || message->fd < 0 || httpd_queue_work(server, send_log_to_websocket, message) != ESP_OK) {
            free(message);
        }
    }
}

esp_err_t start_rest_server(void * pvParameters)
{
    GLOBAL_STATE = (GlobalState *) pvParameters;
    const char * base_path = "";

    bool enter_recovery = false;
    if (init_fs() != ESP_OK) {
        // Unable to initialize the web app filesystem.
        // Enter recovery mode
        enter_recovery = true;
    }

    REST_CHECK(base_path, "wrong base path", err);
    rest_server_context_t * rest_context = calloc(1, sizeof(rest_server_context_t));
    REST_CHECK(rest_context, "No memory for rest context", err);
    strlcpy(rest_context->base_path, base_path, sizeof(rest_context->base_path));

    log_queue = xQueueCreate(MESSAGE_QUEUE_SIZE, sizeof(char *));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;
    // Leave descriptors available for DNS, Stratum and outbound services.
    config.max_open_sockets = 8;
    config.max_uri_handlers = 28;
    config.lru_purge_enable = true;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 30;
    config.keep_alive_interval = 10;
    config.keep_alive_count = 3;
    config.close_fn = http_session_close;

    ESP_LOGI(TAG, "Starting HTTP Server");
    REST_CHECK(httpd_start(&server, &config) == ESP_OK, "Start server failed", err_start);

    httpd_uri_t recovery_explicit_get_uri = {
        .uri = "/recovery", .method = HTTP_GET, .handler = rest_recovery_handler, .user_ctx = rest_context};
    httpd_register_uri_handler(server, &recovery_explicit_get_uri);

    /* URI handler for fetching system info */
    httpd_uri_t system_info_get_uri = {
        .uri = "/api/system/info", .method = HTTP_GET, .handler = GET_system_info, .user_ctx = rest_context};
    httpd_register_uri_handler(server, &system_info_get_uri);

    /* URI handler for fetching ASIC info */
    httpd_uri_t system_asic_get_uri = {
        .uri = "/api/system/asic", .method = HTTP_GET, .handler = GET_system_asic, .user_ctx = rest_context};
    httpd_register_uri_handler(server, &system_asic_get_uri);

    httpd_uri_t system_asic_options_uri = {
        .uri = "/api/system/asic", .method = HTTP_OPTIONS, .handler = handle_options_request, .user_ctx = NULL};
    httpd_register_uri_handler(server, &system_asic_options_uri);

    /* URI handler for WiFi scan */
    httpd_uri_t wifi_scan_get_uri = {
        .uri = "/api/system/wifi/scan", .method = HTTP_GET, .handler = GET_wifi_scan, .user_ctx = rest_context};
    httpd_register_uri_handler(server, &wifi_scan_get_uri);

    /* URI handler for AP info endpoint */
    httpd_uri_t ap_info_get_uri = {.uri = "/api/ap/info", .method = HTTP_GET, .handler = GET_ap_info, .user_ctx = rest_context};
    httpd_register_uri_handler(server, &ap_info_get_uri);

    /* URI handler for IP info page (network status) */
    httpd_uri_t ap_page_get_uri = {.uri = "/ipinfo", .method = HTTP_GET, .handler = rest_ap_page_handler, .user_ctx = rest_context};
    httpd_register_uri_handler(server, &ap_page_get_uri);

    httpd_uri_t swarm_options_uri = {
        .uri = "/api/swarm",
        .method = HTTP_OPTIONS,
        .handler = handle_options_request,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &swarm_options_uri);

    /* URI handlers for the accepted-share LED */
    httpd_uri_t system_led_get_uri = {
        .uri = "/api/system/led", .method = HTTP_GET, .handler = GET_system_led, .user_ctx = rest_context};
    httpd_register_uri_handler(server, &system_led_get_uri);

    httpd_uri_t system_led_patch_uri = {
        .uri = "/api/system/led", .method = HTTP_PATCH, .handler = PATCH_system_led, .user_ctx = rest_context};
    httpd_register_uri_handler(server, &system_led_patch_uri);

    httpd_uri_t system_led_options_uri = {
        .uri = "/api/system/led", .method = HTTP_OPTIONS, .handler = handle_options_request, .user_ctx = NULL};
    httpd_register_uri_handler(server, &system_led_options_uri);

    httpd_uri_t system_restart_uri = {
        .uri = "/api/system/restart", .method = HTTP_POST, .handler = POST_restart, .user_ctx = rest_context};
    httpd_register_uri_handler(server, &system_restart_uri);

    httpd_uri_t system_restart_options_uri = {
        .uri = "/api/system/restart", .method = HTTP_OPTIONS, .handler = handle_options_request, .user_ctx = NULL};
    httpd_register_uri_handler(server, &system_restart_options_uri);

    httpd_uri_t update_system_settings_uri = {
        .uri = "/api/system", .method = HTTP_PATCH, .handler = PATCH_update_settings, .user_ctx = rest_context};
    httpd_register_uri_handler(server, &update_system_settings_uri);

    httpd_uri_t system_options_uri = {
        .uri = "/api/system",
        .method = HTTP_OPTIONS,
        .handler = handle_options_request,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &system_options_uri);

    httpd_uri_t update_post_ota_firmware = {
        .uri = "/api/system/OTA", .method = HTTP_POST, .handler = POST_OTA_update, .user_ctx = NULL};
    httpd_register_uri_handler(server, &update_post_ota_firmware);

    httpd_uri_t update_post_ota_www = {
        .uri = "/api/system/OTAWWW", .method = HTTP_POST, .handler = POST_WWW_update, .user_ctx = NULL};
    httpd_register_uri_handler(server, &update_post_ota_www);

    /* Initialize GitHub OTA module (creates mutex) */
    ota_github_init();

    /* URI handlers for GitHub OTA update */
    httpd_uri_t ota_github_post_uri = {
        .uri = "/api/system/OTA/github", .method = HTTP_POST, .handler = POST_OTA_github, .user_ctx = NULL};
    httpd_register_uri_handler(server, &ota_github_post_uri);

    httpd_uri_t ota_github_get_uri = {
        .uri = "/api/system/OTA/github", .method = HTTP_GET, .handler = GET_OTA_github_status, .user_ctx = NULL};
    httpd_register_uri_handler(server, &ota_github_get_uri);

    httpd_uri_t ota_github_options_uri = {
        .uri = "/api/system/OTA/github", .method = HTTP_OPTIONS, .handler = handle_options_request, .user_ctx = NULL};
    httpd_register_uri_handler(server, &ota_github_options_uri);

    httpd_uri_t ws = {.uri = "/api/ws", .method = HTTP_GET, .handler = echo_handler, .user_ctx = NULL, .is_websocket = true};
    httpd_register_uri_handler(server, &ws);

    if (enter_recovery) {
        /* Make default route serve Recovery */
        httpd_uri_t recovery_implicit_get_uri = {
            .uri = "/*", .method = HTTP_GET, .handler = rest_recovery_handler, .user_ctx = rest_context};
        httpd_register_uri_handler(server, &recovery_implicit_get_uri);

    } else {
        httpd_uri_t api_common_uri = {
            .uri = "/api/*", .method = HTTP_ANY, .handler = rest_api_common_handler, .user_ctx = rest_context};
        httpd_register_uri_handler(server, &api_common_uri);
        /* URI handler for getting web server files */
        httpd_uri_t common_get_uri = {
            .uri = "/*", .method = HTTP_GET, .handler = rest_common_get_handler, .user_ctx = rest_context};
        httpd_register_uri_handler(server, &common_get_uri);
    }

    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);

    // Start websocket log handler thread
    xTaskCreate(&websocket_log_handler, "websocket_log_handler", 4096, NULL, 2, NULL);

    // Start the DNS server that will redirect all queries to the softAP IP
    dns_server_config_t dns_config = DNS_SERVER_CONFIG_SINGLE("*" /* all A queries */, "WIFI_AP_DEF" /* softAP netif ID */);
    start_dns_server(&dns_config);

    return ESP_OK;
err_start:
    free(rest_context);
err:
    return ESP_FAIL;
}
