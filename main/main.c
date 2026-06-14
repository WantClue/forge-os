
#include "esp_event.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "nvs_flash.h"

// #include "protocol_examples_common.h"
#include "main.h"

#include "asic_result_task.h"
#include "asic_task.h"
#include "create_jobs_task.h"
#include "hashrate_monitor_task.h"
#include "esp_netif.h"
#include "system.h"
#include "http_server.h"
#include "nvs_config.h"
#include "serial.h"
#include "stratum_task.h"
#include "i2c_bitforge.h"
#include "adc.h"
#include "nvs_device.h"
#include "self_test.h"
#include "asic.h"

static GlobalState GLOBAL_STATE = {
    .extranonce_str = NULL, 
    .extranonce_2_len = 0, 
    .abandon_work = 0, 
    .version_mask = 0,
    .ASIC_initalized = false
};

static const char * TAG = "bitforge";

static void ap_timeout_task(void * pvParameters)
{
    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;
    
    // Wait 7 minutes (420 seconds)
    vTaskDelay(pdMS_TO_TICKS(420000));
    
    // Check if WiFi is still connected
    bool wifi_connected = (strlen(GLOBAL_STATE->SYSTEM_MODULE.ip_addr_str) > 0 &&
                          strcmp(GLOBAL_STATE->SYSTEM_MODULE.ip_addr_str, "0.0.0.0") != 0);
    
    if (wifi_connected && GLOBAL_STATE->SYSTEM_MODULE.ap_enabled) {
        ESP_LOGI(TAG, "7 minutes elapsed and WiFi connected - turning off AP to save resources");
        wifi_softap_off();
    } else {
        ESP_LOGI(TAG, "7 minutes elapsed but WiFi not connected - keeping AP active");
    }
    
    // Task done, delete itself
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Welcome to the bitforge nano || GTFO!");

    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "No PSRAM available on ESP32 device!");
        GLOBAL_STATE.psram_is_available = false;
    } else {
        GLOBAL_STATE.psram_is_available = true;
    }

    // Init I2C
    ESP_ERROR_CHECK(i2c_bitforge_init());
    ESP_LOGI(TAG, "I2C initialized successfully");

    //wait for I2C to init
    vTaskDelay(100 / portTICK_PERIOD_MS);

    //Init ADC
    ADC_init(&GLOBAL_STATE);

    //initialize the ESP32 NVS
    if (NVSDevice_init() != ESP_OK){
        ESP_LOGE(TAG, "Failed to init NVS");
        return;
    }

    //parse the NVS config into GLOBAL_STATE
    if (NVSDevice_parse_config(&GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse NVS config");
        return;
    }

    if (ASIC_set_device_model(&GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Error setting ASIC model");
        return;
    }

    // Optionally hold the boot button
    // bool pressed = gpio_get_level(CONFIG_GPIO_BUTTON_BOOT) == 0; // LOW when pressed <--- Not suppoerted on Nano
    //should we run the self test?
    if (production_test(&GLOBAL_STATE)){ // || pressed) { // Button not supported
        execute_production_test((void *) &GLOBAL_STATE);
        return;
    }

    SYSTEM_init_system(&GLOBAL_STATE);

    // pull the wifi credentials and hostname out of NVS
    char * wifi_ssid = nvs_config_get_string(NVS_CONFIG_WIFI_SSID, WIFI_SSID);
    char * wifi_pass = nvs_config_get_string(NVS_CONFIG_WIFI_PASS, WIFI_PASS);
    char * hostname  = nvs_config_get_string(NVS_CONFIG_HOSTNAME, HOSTNAME);

    // copy the wifi ssid to the global state
    strncpy(GLOBAL_STATE.SYSTEM_MODULE.ssid, wifi_ssid, sizeof(GLOBAL_STATE.SYSTEM_MODULE.ssid));
    GLOBAL_STATE.SYSTEM_MODULE.ssid[sizeof(GLOBAL_STATE.SYSTEM_MODULE.ssid)-1] = 0;

    // init AP and connect to wifi
    wifi_init(wifi_ssid, wifi_pass, hostname, GLOBAL_STATE.SYSTEM_MODULE.ip_addr_str);

    generate_ssid(GLOBAL_STATE.SYSTEM_MODULE.ap_ssid);

    SYSTEM_init_peripherals(&GLOBAL_STATE);

    xTaskCreate(POWER_MANAGEMENT_task, "power management", 8192, (void *) &GLOBAL_STATE, 10,
                &GLOBAL_STATE.power_management_task_handle);

    //start the API for AxeOS
    start_rest_server((void *) &GLOBAL_STATE);
    EventBits_t result_bits = wifi_connect();

    if (result_bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to SSID: %s", wifi_ssid);
        strncpy(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, "Connected!", 20);
    } else if (result_bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID: %s", wifi_ssid);

        strncpy(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, "Failed to connect", 20);
        // User might be trying to configure with AP, just chill here
        ESP_LOGI(TAG, "Finished, waiting for user input.");
        while (1) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
        strncpy(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, "unexpected error", 20);
        // User might be trying to configure with AP, just chill here
        ESP_LOGI(TAG, "Finished, waiting for user input.");
        while (1) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    }

    free(wifi_ssid);
    free(wifi_pass);
    free(hostname);

    GLOBAL_STATE.new_stratum_version_rolling_msg = false;

    // Keep AP active for dual-mode operation (AP + STA)
    // wifi_softap_off();  // Commented out to maintain AP access
    ESP_LOGI(TAG, "AP remains active for dual-mode operation");

    // Create task to turn off AP after 7 minutes if WiFi is connected
    xTaskCreate(&ap_timeout_task, "ap_timeout", 2048, (void *) &GLOBAL_STATE, 1, NULL);

    queue_init(&GLOBAL_STATE.stratum_queue);
    queue_init(&GLOBAL_STATE.ASIC_jobs_queue);

    SERIAL_init();

    if (ASIC_init(&GLOBAL_STATE) == 0) {
        GLOBAL_STATE.SYSTEM_MODULE.asic_status = "Chip count 0";
        ESP_LOGE(TAG, "Chip count 0");
        return;
    }

    SERIAL_set_baud(ASIC_set_max_baud(&GLOBAL_STATE));
    SERIAL_clear_buffer();

    GLOBAL_STATE.ASIC_initalized = true;
    GLOBAL_STATE.mining_control_ready = true;

    xTaskCreate(stratum_task, "stratum admin", 8192, (void *) &GLOBAL_STATE, 5, NULL);
    xTaskCreate(create_jobs_task, "stratum miner", 8192, (void *) &GLOBAL_STATE, 10, NULL);
    xTaskCreate(ASIC_task, "asic", 8192, (void *) &GLOBAL_STATE, 10, NULL);
    xTaskCreate(ASIC_result_task, "asic result", 8192, (void *) &GLOBAL_STATE, 15, NULL);
    xTaskCreate(hashrate_monitor_task, "hashrate monitor", 4096, (void *) &GLOBAL_STATE, 5, NULL);
}

void MINER_set_wifi_status(wifi_status_t status, int retry_count, int reason)
{
    switch(status) {
        case WIFI_CONNECTING:
            snprintf(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, 20, "Connecting...");
            return;
        case WIFI_CONNECTED:
            snprintf(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, 20, "Connected!");
            return;
        case WIFI_RETRYING:
            // See https://github.com/espressif/esp-idf/blob/master/components/esp_wifi/include/esp_wifi_types_generic.h for codes
            switch(reason) {
                case 201:
                    snprintf(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, 20, "No AP found (%d)", retry_count);
                    return;
                case 15:
                case 205:
                    snprintf(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, 20, "Password error (%d)", retry_count);
                    return;
                default:
                    snprintf(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, 20, "Error %d (%d)", reason, retry_count);
                    return;
            }
    }
    ESP_LOGW(TAG, "Unknown status: %d", status);
}

void MINER_set_ap_status(bool enabled) {
    GLOBAL_STATE.SYSTEM_MODULE.ap_enabled = enabled;
}
