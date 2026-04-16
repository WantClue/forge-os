#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

#include "cJSON.h"
#include "handler_ota_github.h"
#include "http_server.h"

static const char *TAG = "ota_github";

#define GITHUB_URL_PREFIX "https://github.com/wantclue/forge-os/"
#define OTA_BUF_SIZE 2048
#define MAX_URL_LEN 512
#define MAX_REDIRECT 8

// OTA update steps
typedef enum {
    OTA_STEP_IDLE = 0,
    OTA_STEP_DOWNLOADING_FW,
    OTA_STEP_FLASHING_FW,
    OTA_STEP_DOWNLOADING_WWW,
    OTA_STEP_FLASHING_WWW,
    OTA_STEP_REBOOTING,
    OTA_STEP_ERROR,
} ota_step_t;

// OTA state shared between task and HTTP handlers
static struct {
    SemaphoreHandle_t mutex;
    bool running;
    ota_step_t step;
    int progress;       // 0-100
    char error_msg[64];
    char fw_url[MAX_URL_LEN];
    char www_url[MAX_URL_LEN];
    TaskHandle_t task_handle;
} ota_state = {0};

void ota_github_init(void)
{
    ota_state.mutex = xSemaphoreCreateMutex();
    assert(ota_state.mutex != NULL);
}

static const char *step_to_string(ota_step_t step)
{
    switch (step) {
        case OTA_STEP_IDLE:             return "idle";
        case OTA_STEP_DOWNLOADING_FW:   return "downloading_fw";
        case OTA_STEP_FLASHING_FW:      return "flashing_fw";
        case OTA_STEP_DOWNLOADING_WWW:  return "downloading_www";
        case OTA_STEP_FLASHING_WWW:     return "flashing_www";
        case OTA_STEP_REBOOTING:        return "rebooting";
        case OTA_STEP_ERROR:            return "error";
        default:                        return "unknown";
    }
}

static void set_step(ota_step_t step)
{
    xSemaphoreTake(ota_state.mutex, portMAX_DELAY);
    ota_state.step = step;
    xSemaphoreGive(ota_state.mutex);
}

static void set_progress(int progress)
{
    xSemaphoreTake(ota_state.mutex, portMAX_DELAY);
    ota_state.progress = progress;
    xSemaphoreGive(ota_state.mutex);
}

static void set_error(const char *msg)
{
    xSemaphoreTake(ota_state.mutex, portMAX_DELAY);
    ota_state.step = OTA_STEP_ERROR;
    strncpy(ota_state.error_msg, msg, sizeof(ota_state.error_msg) - 1);
    ota_state.error_msg[sizeof(ota_state.error_msg) - 1] = '\0';
    ota_state.running = false;
    xSemaphoreGive(ota_state.mutex);
}

// Follow GitHub redirects and download binary data into a buffer or directly to OTA/partition
// Returns content length on success, -1 on error

typedef struct {
    // For firmware OTA writes
    esp_ota_handle_t ota_handle;
    const esp_partition_t *partition;
    bool use_ota_write;

    // For www partition writes (buffered to PSRAM first)
    uint8_t *buffer;
    int buffer_size;
    int bytes_written;

    // Progress tracking
    int total_expected;
    int total_downloaded;
    void (*progress_cb)(int downloaded, int total);
} download_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    download_ctx_t *ctx = (download_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx != NULL) {
        if (ctx->use_ota_write) {
            if (esp_ota_write(ctx->ota_handle, evt->data, evt->data_len) != ESP_OK) {
                ESP_LOGE(TAG, "OTA write failed");
                return ESP_FAIL;
            }
        } else if (ctx->buffer != NULL) {
            if (ctx->bytes_written + evt->data_len > ctx->buffer_size) {
                ESP_LOGE(TAG, "Buffer overflow: %d + %d > %d",
                         ctx->bytes_written, evt->data_len, ctx->buffer_size);
                return ESP_FAIL;
            }
            memcpy(ctx->buffer + ctx->bytes_written, evt->data, evt->data_len);
        }
        ctx->bytes_written += evt->data_len;
        ctx->total_downloaded += evt->data_len;

        if (ctx->progress_cb && ctx->total_expected > 0) {
            ctx->progress_cb(ctx->total_downloaded, ctx->total_expected);
        }
    }
    return ESP_OK;
}

static int download_from_github(const char *url, download_ctx_t *ctx)
{
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = OTA_BUF_SIZE,
        .buffer_size_tx = OTA_BUF_SIZE,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return -1;
    }

    // Disable auto-redirect so we can follow manually and validate URLs
    esp_http_client_set_redirection(client);

    esp_err_t err;
    int status_code;
    int content_length = -1;
    int redirects = 0;

    while (redirects < MAX_REDIRECT) {
        ctx->bytes_written = 0;

        err = esp_http_client_perform(client);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
            break;
        }

        status_code = esp_http_client_get_status_code(client);
        content_length = esp_http_client_get_content_length(client);

        if (status_code == 200) {
            ESP_LOGI(TAG, "Download complete, %d bytes", ctx->bytes_written);
            esp_http_client_cleanup(client);
            return ctx->bytes_written;
        }

        if (status_code == 301 || status_code == 302 || status_code == 307 || status_code == 308) {
            // esp_http_client stores redirect location internally
            // Just set redirect and retry
            err = esp_http_client_set_redirection(client);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set redirect");
                break;
            }
            redirects++;
            ESP_LOGI(TAG, "Following redirect #%d", redirects);
            continue;
        }

        ESP_LOGE(TAG, "Unexpected HTTP status: %d", status_code);
        break;
    }

    esp_http_client_cleanup(client);
    return -1;
}

// Firmware size: up to 4MB, WWW size: up to 3MB
#define FW_MAX_SIZE  (4 * 1024 * 1024)
#define WWW_MAX_SIZE (3 * 1024 * 1024)

static void fw_progress_cb(int downloaded, int total)
{
    // Firmware is 0-50% of total progress
    int pct = (downloaded * 50) / total;
    set_progress(pct);
}

static void www_progress_cb(int downloaded, int total)
{
    // WWW download is 50-80% of total progress
    int pct = 50 + (downloaded * 30) / total;
    set_progress(pct);
}

static void ota_github_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting GitHub OTA update");
    ESP_LOGI(TAG, "FW URL: %s", ota_state.fw_url);
    ESP_LOGI(TAG, "WWW URL: %s", ota_state.www_url);

    // ---- Step 1: Download & flash firmware ----
    set_step(OTA_STEP_DOWNLOADING_FW);
    set_progress(0);

    const esp_partition_t *ota_partition = esp_ota_get_next_update_partition(NULL);
    if (ota_partition == NULL) {
        set_error("No OTA partition found");
        vTaskDelete(NULL);
        return;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        set_error("OTA begin failed");
        vTaskDelete(NULL);
        return;
    }

    download_ctx_t fw_ctx = {
        .ota_handle = ota_handle,
        .partition = ota_partition,
        .use_ota_write = true,
        .buffer = NULL,
        .buffer_size = 0,
        .bytes_written = 0,
        .total_expected = FW_MAX_SIZE,
        .total_downloaded = 0,
        .progress_cb = fw_progress_cb,
    };

    set_step(OTA_STEP_FLASHING_FW);
    int fw_size = download_from_github(ota_state.fw_url, &fw_ctx);
    if (fw_size <= 0) {
        esp_ota_abort(ota_handle);
        set_error("Firmware download failed");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Firmware downloaded: %d bytes", fw_size);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        set_error("Firmware validation failed");
        vTaskDelete(NULL);
        return;
    }

    err = esp_ota_set_boot_partition(ota_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        set_error("Set boot partition failed");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Firmware flashed successfully");
    set_progress(50);

    // ---- Step 2: Download WWW to PSRAM buffer ----
    set_step(OTA_STEP_DOWNLOADING_WWW);

    const esp_partition_t *www_partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "www");
    if (www_partition == NULL) {
        set_error("WWW partition not found");
        vTaskDelete(NULL);
        return;
    }

    // Allocate buffer in PSRAM for www binary
    uint8_t *www_buf = heap_caps_malloc(WWW_MAX_SIZE, MALLOC_CAP_SPIRAM);
    if (www_buf == NULL) {
        ESP_LOGW(TAG, "PSRAM alloc failed, trying internal RAM with smaller buffer");
        // Fallback: skip www update if no PSRAM available
        set_error("Not enough memory for WWW update");
        vTaskDelete(NULL);
        return;
    }

    download_ctx_t www_ctx = {
        .use_ota_write = false,
        .buffer = www_buf,
        .buffer_size = WWW_MAX_SIZE,
        .bytes_written = 0,
        .total_expected = WWW_MAX_SIZE,
        .total_downloaded = 0,
        .progress_cb = www_progress_cb,
    };

    int www_size = download_from_github(ota_state.www_url, &www_ctx);
    if (www_size <= 0) {
        free(www_buf);
        set_error("WWW download failed");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "WWW downloaded: %d bytes", www_size);
    set_progress(80);

    // ---- Step 3: Flash WWW to SPIFFS partition ----
    set_step(OTA_STEP_FLASHING_WWW);

    if (www_size > www_partition->size) {
        free(www_buf);
        set_error("WWW binary too large");
        vTaskDelete(NULL);
        return;
    }

    err = esp_partition_erase_range(www_partition, 0, www_partition->size);
    if (err != ESP_OK) {
        free(www_buf);
        ESP_LOGE(TAG, "WWW partition erase failed: %s", esp_err_to_name(err));
        set_error("WWW erase failed");
        vTaskDelete(NULL);
        return;
    }

    // Write in chunks
    int offset = 0;
    while (offset < www_size) {
        int chunk = MIN(OTA_BUF_SIZE, www_size - offset);
        err = esp_partition_write(www_partition, offset, www_buf + offset, chunk);
        if (err != ESP_OK) {
            free(www_buf);
            ESP_LOGE(TAG, "WWW write failed at offset %d: %s", offset, esp_err_to_name(err));
            set_error("WWW write failed");
            vTaskDelete(NULL);
            return;
        }
        offset += chunk;

        // Progress: 80-95%
        int pct = 80 + (offset * 15) / www_size;
        set_progress(pct);
    }

    free(www_buf);
    ESP_LOGI(TAG, "WWW flashed successfully");
    set_progress(100);

    // ---- Step 4: Reboot ----
    set_step(OTA_STEP_REBOOTING);

    // Wait a bit so frontend can see "rebooting" status
    vTaskDelay(3000 / portTICK_PERIOD_MS);

    ESP_LOGI(TAG, "Rebooting after GitHub OTA update");
    esp_restart();

    // Never reached
    vTaskDelete(NULL);
}

esp_err_t POST_OTA_github(httpd_req_t *req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    // Read request body
    char buf[1024];
    int total_len = req->content_len;
    if (total_len <= 0 || total_len >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
        return ESP_OK;
    }

    int cur_len = 0;
    while (cur_len < total_len) {
        int received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read request");
            return ESP_OK;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    // Parse JSON
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_OK;
    }

    cJSON *fw_url_item = cJSON_GetObjectItem(root, "fw_url");
    cJSON *www_url_item = cJSON_GetObjectItem(root, "www_url");

    if (!cJSON_IsString(fw_url_item) || !cJSON_IsString(www_url_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing fw_url or www_url");
        return ESP_OK;
    }

    // Validate URLs - must start with our GitHub repo (case-insensitive: GitHub normalises owner casing)
    if (strncasecmp(fw_url_item->valuestring, GITHUB_URL_PREFIX, strlen(GITHUB_URL_PREFIX)) != 0 ||
        strncasecmp(www_url_item->valuestring, GITHUB_URL_PREFIX, strlen(GITHUB_URL_PREFIX)) != 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URL: must be from wantclue/forge-os");
        return ESP_OK;
    }

    // Reject path traversal attempts
    if (strstr(fw_url_item->valuestring, "..") != NULL ||
        strstr(www_url_item->valuestring, "..") != NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URL");
        return ESP_OK;
    }

    // Atomically check running flag and claim the update slot
    xSemaphoreTake(ota_state.mutex, portMAX_DELAY);
    if (ota_state.running) {
        xSemaphoreGive(ota_state.mutex);
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Update already in progress");
        return ESP_OK;
    }
    strncpy(ota_state.fw_url, fw_url_item->valuestring, MAX_URL_LEN - 1);
    ota_state.fw_url[MAX_URL_LEN - 1] = '\0';
    strncpy(ota_state.www_url, www_url_item->valuestring, MAX_URL_LEN - 1);
    ota_state.www_url[MAX_URL_LEN - 1] = '\0';
    ota_state.running = true;
    ota_state.step = OTA_STEP_IDLE;
    ota_state.progress = 0;
    ota_state.error_msg[0] = '\0';
    xSemaphoreGive(ota_state.mutex);

    cJSON_Delete(root);

    // Spawn the OTA task with enough stack for TLS + HTTP client
    BaseType_t ret = xTaskCreate(ota_github_task, "ota_github", 8192, NULL, 5, &ota_state.task_handle);
    if (ret != pdPASS) {
        xSemaphoreTake(ota_state.mutex, portMAX_DELAY);
        ota_state.running = false;
        xSemaphoreGive(ota_state.mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start update task");
        return ESP_OK;
    }

    // Return immediately
    httpd_resp_set_type(req, "application/json");
    const char *resp = "{\"status\":\"started\"}";
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

esp_err_t GET_OTA_github_status(httpd_req_t *req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    // Prevent caching
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");

    xSemaphoreTake(ota_state.mutex, portMAX_DELAY);
    ota_step_t step = ota_state.step;
    int progress = ota_state.progress;
    bool running = ota_state.running;
    char error_msg[64];
    strncpy(error_msg, ota_state.error_msg, sizeof(error_msg));
    xSemaphoreGive(ota_state.mutex);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "running", running);
    cJSON_AddStringToObject(root, "step", step_to_string(step));
    cJSON_AddNumberToObject(root, "progress", progress);
    if (step == OTA_STEP_ERROR) {
        cJSON_AddStringToObject(root, "error", error_msg);
    }

    httpd_resp_set_type(req, "application/json");
    const char *response = cJSON_Print(root);
    cJSON_Delete(root);
    if (response == NULL) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    httpd_resp_sendstr(req, response);
    free((void *)response);
    return ESP_OK;
}
