#include <string.h>
#include <inttypes.h>
#include "driver/gpio.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "self_test.h"
#include "i2c_bitforge.h"
#include "PAC9544.h"
#include "DS4432U.h"
#include "INA260.h"
#include "adc.h"
#include "nvs_config.h"
#include "nvs_flash.h"
#include "input.h"
#include "vcore.h"
#include "utils.h"
#include "TPS546.h"
#include "esp_psram.h"
#include "power.h"
#include "asic.h"
#include "esp_err.h"
#include "esp_check.h"
#include "ThermalMonitoring.h"
#include "hashrate_monitor_task.h"

/*******************************************************************************
 * LED FAILURE DIAGNOSTIC CODES
 * 
 * After self-test completion, LEDs indicate the test result:
 * 
 * LED Pattern              | Status            | Cause
 * -------------------------|-------------------|----------------------------------
 * LED1,LED2: ON BLINKING   | PASS              | All tests passed successfully
 * LED1: ON,  LED2: OFF     | PERIPHERAL_FAILURE| PSRAM, Input, or Peripheral
 *                          |                   | initialization failed
 * LED1: OFF, LED2: ON      | ASIC_FAILURE      | ASIC detection, hashrate, or
 *                          |                   | reference voltage test failed
 * LED1: ON,  LED2: ON      | POWER_FAILURE     | Voltage regulator or power
 *                          |                   | consumption test failed
 * 
 * Note: LEDs use active-low logic (GPIO LOW = LED ON, GPIO HIGH = LED OFF)
 * 
 * To reset after failure: Press and hold button or press RESET
 ******************************************************************************/


#define BLINK_GPIO_1 9
#define BLINK_GPIO_2 12

#define GPIO_ASIC_ENABLE CONFIG_GPIO_ASIC_ENABLE

#define TESTS_FAILED 0
#define TESTS_PASSED 1

/////Test Constants/////
//Test Fan Speed
#define FAN_SPEED_TARGET_MIN 900 //RPM

//Test Core Voltage
#define CORE_VOLTAGE_TARGET_MIN 950 //mV
#define CORE_VOLTAGE_TARGET_MAX 1300 //mV

//Test Reference Voltage
#define REFERENCE_VOLTAGE_1V2_MIN 1050 //mV
#define REFERENCE_VOLTAGE_1V2_MAX 1350 //mV
#define REFERENCE_VOLTAGE_0V8_MIN 650  //mV
#define REFERENCE_VOLTAGE_0V8_MAX 950  //mV

//Test Power Consumption
#define POWER_CONSUMPTION_NANO 37     //watts
#define POWER_CONSUMPTION_MARGIN 10    //+/- watts

static const char * TAG = "self_test";

SemaphoreHandle_t BootSemaphore;

static void tests_done(GlobalState * GLOBAL_STATE, bool test_result,  TEST_FAILED_CAUSE cause);

static uint8_t s_led_state = 0;
static bool initialized = false;

static esp_err_t configure_led(void)
{
    gpio_reset_pin(BLINK_GPIO_1);
    gpio_reset_pin(BLINK_GPIO_2);
    gpio_set_direction(BLINK_GPIO_1, GPIO_MODE_OUTPUT);
    gpio_set_direction(BLINK_GPIO_2, GPIO_MODE_OUTPUT);

    gpio_set_level(BLINK_GPIO_1, 1);
    gpio_set_level(BLINK_GPIO_2, 1);

    return ESP_OK;
}


static void switch_led(int num, uint8_t state)
{
    switch(num)
    {
        case 1:
            gpio_set_level(BLINK_GPIO_1, !state);
            break;
        case 2:
            gpio_set_level(BLINK_GPIO_2, !state);
            break;
        default:
            break;
    }
}

bool production_test(GlobalState * GLOBAL_STATE) {
    bool is_max = GLOBAL_STATE->asic_model == ASIC_UNKNOWN;
    uint64_t best_diff = nvs_config_get_u64(NVS_CONFIG_BEST_DIFF, 0);
    uint16_t production_test = nvs_config_get_u16(NVS_CONFIG_PRODUCTION_TEST, 0);
    if (production_test == 1 && !is_max && best_diff < 1) {
        return true;
    }
    return false;
}

static void reset_self_test(void) {
    ESP_LOGI(TAG, "Long press detected...");
    xSemaphoreGive(BootSemaphore);
}

static esp_err_t test_fan_sense(GlobalState * GLOBAL_STATE)
{
    uint16_t fan_speed_1 = 0;
    uint16_t fan_speed_2 = 0;
    switch (GLOBAL_STATE->device_model) {
        case BITFORGE_NANO:
            fan_speed_1 = Thermal_getFanSpeed();
            fan_speed_2 = 1001;
            break;
        default:
    }
    ESP_LOGI(TAG, "fanSpeed: %d", fan_speed_1);
    if (fan_speed_1 > FAN_SPEED_TARGET_MIN) {
        ESP_LOGI(TAG, "fanSpeed: %d", fan_speed_2);
        if (fan_speed_2 > FAN_SPEED_TARGET_MIN) {
            return ESP_OK;
        }
    }

    ESP_LOGE(TAG, "FAN test failed!");
    return ESP_FAIL;
}

static esp_err_t test_INA260_power_consumption(int target_power, int margin)
{
    float power = INA260_read_power() / 1000;
    ESP_LOGI(TAG, "Power: %f", power);
    if (power > target_power -margin && power < target_power +margin) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t test_TPS546_power_consumption(int target_power, int margin)
{
    float voltage = TPS546_get_vout();
    float current = TPS546_get_iout();
    float power = voltage * current;
    ESP_LOGI(TAG, "Power: %f, Voltage: %f, Current %f", power, voltage, current);
    if (power > target_power -margin && power < target_power +margin) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t test_reference_voltages(GlobalState * GLOBAL_STATE) 
{
    uint16_t _1V2_voltage = ADC_read(V_1V2_REF, GLOBAL_STATE);
    ESP_LOGI(TAG, "1V2 reference voltage: %u", _1V2_voltage); 
    if (_1V2_voltage < REFERENCE_VOLTAGE_1V2_MIN && _1V2_voltage > REFERENCE_VOLTAGE_1V2_MAX) {
        ESP_LOGE(TAG, "1V2 reference voltage TEST FAIL, INCORRECT REFERENCE VOLTAGE");
        return ESP_FAIL;
    }

    uint16_t _0V8_voltage = ADC_read(V_0V8_REF, GLOBAL_STATE);
    ESP_LOGI(TAG, "0V8 reference voltage: %u", _0V8_voltage);
    if (_0V8_voltage < REFERENCE_VOLTAGE_0V8_MIN && _0V8_voltage > REFERENCE_VOLTAGE_0V8_MAX) {
        ESP_LOGE(TAG, "0V8 reference voltage TEST FAIL, INCORRECT REFERENCE VOLTAGE");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t test_core_voltage(GlobalState * GLOBAL_STATE)
{
    uint16_t core_voltage = VCORE_get_voltage_mv(GLOBAL_STATE);
    ESP_LOGI(TAG, "Voltage: %u", core_voltage);

    if (core_voltage > CORE_VOLTAGE_TARGET_MIN && core_voltage < CORE_VOLTAGE_TARGET_MAX) {
        return ESP_OK;
    }
    //tests failed
    ESP_LOGE(TAG, "Core Voltage TEST FAIL, INCORRECT CORE VOLTAGE, voltage measured: %u", core_voltage);
    return ESP_FAIL;
}

esp_err_t test_input(GlobalState * GLOBAL_STATE) {
    switch (GLOBAL_STATE->device_model) {
        case BITFORGE_NANO:
        default:
    }

    return ESP_OK;
}

esp_err_t init_voltage_regulator(GlobalState * GLOBAL_STATE) {
    ESP_RETURN_ON_ERROR(VCORE_init(GLOBAL_STATE), TAG, "VCORE init failed!");
    //ESP_RETURN_ON_ERROR(VCORE_set_voltage(0U, GLOBAL_STATE), TAG, "VCORE set voltage failed!");
    ESP_RETURN_ON_ERROR(VCORE_set_voltage(nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE, CONFIG_ASIC_VOLTAGE) / 1000.0, GLOBAL_STATE), TAG, "VCORE set voltage failed!");
    
    return ESP_OK;
}

esp_err_t test_vreg_faults(GlobalState * GLOBAL_STATE) {
    ESP_RETURN_ON_ERROR(VCORE_check_fault(GLOBAL_STATE), TAG, "VCORE check fault failed!");

    if (GLOBAL_STATE->SYSTEM_MODULE.power_fault) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t test_voltage_regulator(GlobalState * GLOBAL_STATE) {
    
    //enable the voltage regulator GPIO on HW that supports it
    switch (GLOBAL_STATE->device_model) {
        case BITFORGE_NANO:
        default:
    }

    if (init_voltage_regulator(GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "VCORE init failed!");
        return ESP_FAIL;
    }

    // VCore regulator testing
    switch (GLOBAL_STATE->device_model) {
        case BITFORGE_NANO:
            break;
        default:
    }

    ESP_LOGI(TAG, "Voltage Regulator test success!");
    return ESP_OK;
}

esp_err_t test_init_peripherals(GlobalState * GLOBAL_STATE) {
    
    //Init the EMC2101 fan and temperature monitoring
    switch (GLOBAL_STATE->device_model) {
        case BITFORGE_NANO:
            Thermal_init(GLOBAL_STATE);
            break;
        default:
    }

    //initialize the INA260, if we have one.
    switch (GLOBAL_STATE->device_model) {
        case BITFORGE_NANO:
            ESP_RETURN_ON_ERROR(INA260_init(), TAG, "INA260 init failed!");
            break;
        default:
    }

    ESP_LOGI(TAG, "Peripherals init success!");
    return ESP_OK;
}

esp_err_t test_psram(GlobalState * GLOBAL_STATE){
    if(!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "No PSRAM available on ESP32!");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief Perform a self-test of the system.
 *
 * This function is intended to be run as a task and will execute a series of 
 * diagnostic tests to ensure the system is functioning correctly.
 *
 * @param pvParameters Pointer to the parameters passed to the task (if any).
 */
void execute_production_test(void * pvParameters)
{
    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    ESP_LOGI(TAG, "Running Self Tests");

    // Create a binary semaphore. Has to exist before the first tests_done() call,
    // which blocks on it forever when a test fails.
    BootSemaphore = xSemaphoreCreateBinary();

    if (BootSemaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return;
    }

    gpio_install_isr_service(0);

    // A long press on the boot button is the only way out of a failed self test
    if (input_init(NULL, reset_self_test) != ESP_OK) {
        ESP_LOGE(TAG, "Input init failed!");
    }

    if (configure_led() != ESP_OK) {
        ESP_LOGE(TAG, "LED config failed!");
        tests_done(GLOBAL_STATE, TESTS_FAILED, PERIPHERAL_FAILURE);
    }

    //Run PSRAM test
    if(test_psram(GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "NO PSRAM on device!");
        tests_done(GLOBAL_STATE, TESTS_FAILED, PERIPHERAL_FAILURE);
    }

    //Run input tests
    if (test_input(GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Input test failed!");
        tests_done(GLOBAL_STATE, TESTS_FAILED, PERIPHERAL_FAILURE);
    }

    //Voltage Regulator Testing
    if (test_voltage_regulator(GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Voltage Regulator test failed!");
        tests_done(GLOBAL_STATE, TESTS_FAILED, POWER_FAILURE);
    }

    //Init peripherals EMC2101 and INA260 (if present)
    if (test_init_peripherals(GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Peripherals init failed!");
        tests_done(GLOBAL_STATE, TESTS_FAILED, PERIPHERAL_FAILURE);
    }

    Thermal_setFanSpeedPercent(1.0f);

    switch (GLOBAL_STATE->device_model) {
        case BITFORGE_NANO:
            if (test_reference_voltages(GLOBAL_STATE) != ESP_OK) {
                tests_done(GLOBAL_STATE, TESTS_FAILED, ASIC_FAILURE);
            }
            break;
        default:
    }

    vTaskDelay(1000 / portTICK_PERIOD_MS);

    //test for number of ASICs
    if (SERIAL_init() != ESP_OK) {
        ESP_LOGE(TAG, "SERIAL init failed!");
        tests_done(GLOBAL_STATE, TESTS_FAILED, ASIC_FAILURE);
    }

    uint8_t chips_detected = ASIC_init(GLOBAL_STATE);
    uint8_t chips_expected = ASIC_get_asic_count(GLOBAL_STATE);
    ESP_LOGI(TAG, "%u chips detected, %u expected", chips_detected, chips_expected);

    if (chips_detected != chips_expected) {
        ESP_LOGE(TAG, "SELF TEST FAIL, %d of %d CHIPS DETECTED", chips_detected, chips_expected);
        char error_buf[20];
        snprintf(error_buf, 20, "ASIC:FAIL %d CHIPS", chips_detected);
        tests_done(GLOBAL_STATE, TESTS_FAILED, ASIC_FAILURE);
    }

    PAC9544_selectChannel(2);
    float temp_ASIC_1 = Thermal_getAsicChipTemp(GLOBAL_STATE);
    ESP_LOGI(TAG, "External Temp of EMC2101_ASIC1: %f", temp_ASIC_1);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    PAC9544_selectChannel(3);
    float temp_ASIC_2 = Thermal_getAsicChipTemp(GLOBAL_STATE);
    ESP_LOGI(TAG, "External Temp of EMC2101_ASIC2: %f", temp_ASIC_2);

    //test for voltage regulator faults
    if (test_vreg_faults(GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "VCORE check fault failed!");
        tests_done(GLOBAL_STATE, TESTS_FAILED, POWER_FAILURE);
    }

    ESP_LOGI(TAG, "Initializing ASIC frequency to %d MHz",
             (int)GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value);
    
    int baud = ASIC_set_max_baud(GLOBAL_STATE);
    vTaskDelay(10 / portTICK_PERIOD_MS);

    if (SERIAL_set_baud(baud) != ESP_OK) {
        ESP_LOGE(TAG, "SERIAL set baud failed!");
        tests_done(GLOBAL_STATE, TESTS_FAILED, ASIC_FAILURE);
    }

    GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs = malloc(sizeof(bm_job *) * 128);
    GLOBAL_STATE->valid_jobs = malloc(sizeof(uint8_t) * 128);

    for (int i = 0; i < 128; i++) {
        GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[i] = NULL;
        GLOBAL_STATE->valid_jobs[i] = 0;
    }

    vTaskDelay(1000 / portTICK_PERIOD_MS);

    mining_notify notify_message;
    notify_message.job_id = 0;
    notify_message.prev_block_hash = "0c859545a3498373a57452fac22eb7113df2a465000543520000000000000000";
    notify_message.version = 0x20000004;
    notify_message.target = 0x1705ae3a;
    notify_message.ntime = 0x647025b5;
    notify_message.difficulty = 1000000;

    const char * coinbase_tx = "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff4b0389130cfab"
                               "e6d6d5cbab26a2599e92916edec"
                               "5657a94a0708ddb970f5c45b5d12905085617eff8e010000000000000031650707758de07b010000000000001cfd703"
                               "8212f736c7573682f0000000003"
                               "79ad0c2a000000001976a9147c154ed1dc59609e3d26abb2df2ea3d587cd8c4188ac00000000000000002c6a4c29525"
                               "34b424c4f434b3ae725d3994b81"
                               "1572c1f345deb98b56b465ef8e153ecbbd27fa37bf1b005161380000000000000000266a24aa21a9ed63b06a7946b19"
                               "0a3fda1d76165b25c9b883bcc66"
                               "21b040773050ee2a1bb18f1800000000";
    uint8_t merkles[13][32];
    int num_merkles = 13;

    hex2bin("2b77d9e413e8121cd7a17ff46029591051d0922bd90b2b2a38811af1cb57a2b2", merkles[0], 32);
    hex2bin("5c8874cef00f3a233939516950e160949ef327891c9090467cead995441d22c5", merkles[1], 32);
    hex2bin("2d91ff8e19ac5fa69a40081f26c5852d366d608b04d2efe0d5b65d111d0d8074", merkles[2], 32);
    hex2bin("0ae96f609ad2264112a0b2dfb65624bedbcea3b036a59c0173394bba3a74e887", merkles[3], 32);
    hex2bin("e62172e63973d69574a82828aeb5711fc5ff97946db10fc7ec32830b24df7bde", merkles[4], 32);
    hex2bin("adb49456453aab49549a9eb46bb26787fb538e0a5f656992275194c04651ec97", merkles[5], 32);
    hex2bin("a7bc56d04d2672a8683892d6c8d376c73d250a4871fdf6f57019bcc737d6d2c2", merkles[6], 32);
    hex2bin("d94eceb8182b4f418cd071e93ec2a8993a0898d4c93bc33d9302f60dbbd0ed10", merkles[7], 32);
    hex2bin("5ad7788b8c66f8f50d332b88a80077ce10e54281ca472b4ed9bbbbcb6cf99083", merkles[8], 32);
    hex2bin("9f9d784b33df1b3ed3edb4211afc0dc1909af9758c6f8267e469f5148ed04809", merkles[9], 32);
    hex2bin("48fd17affa76b23e6fb2257df30374da839d6cb264656a82e34b350722b05123", merkles[10], 32);
    hex2bin("c4f5ab01913fc186d550c1a28f3f3e9ffaca2016b961a6a751f8cca0089df924", merkles[11], 32);
    hex2bin("cff737e1d00176dd6bbfa73071adbb370f227cfb5fba186562e4060fcec877e1", merkles[12], 32);

    char * merkle_root = calculate_merkle_root_hash(coinbase_tx, merkles, num_merkles);

    bm_job job = construct_bm_job(&notify_message, merkle_root, 0x1fffe000);

    uint8_t difficulty = 8;
    ASIC_set_job_difficulty_mask(GLOBAL_STATE, difficulty);

    ESP_LOGI(TAG, "Sending work");
    ASIC_send_work(GLOBAL_STATE, &job);
    
    // Give chips time to receive and start processing work
    ESP_LOGI(TAG, "Waiting for ASICs to start hashing...");
    vTaskDelay(500 / portTICK_PERIOD_MS);
    
    uint32_t start_ms = esp_timer_get_time() / 1000;
    uint32_t duration_ms = 0;
    uint32_t counter = 0;
    float hashrate = 0;
    uint32_t hashtest_ms = 5000;

    ESP_LOGI(TAG, "Measuring hashrate for 5 seconds...");
    while (duration_ms < hashtest_ms) {
        task_result * asic_result = ASIC_process_work(GLOBAL_STATE);
        if (asic_result != NULL) {
            // check the nonce difficulty
            double nonce_diff = test_nonce_value(&job, asic_result->nonce, asic_result->rolled_version);
            counter += 8;  // DIFFICULTY = 8 (matches ESP-Miner-WantClue)
            duration_ms = (esp_timer_get_time() / 1000) - start_ms;
            hashrate = hash_counter_to_ghs(duration_ms, counter);

            // Log every 50 nonces to avoid watchdog
            if ((counter / 8) % 50 == 0) {
                ESP_LOGI(TAG, "Nonce %lu diff %.2f", (unsigned long)asic_result->nonce, nonce_diff);
                ESP_LOGI(TAG, "%.2f GH/s, duration %"PRIu32"ms", hashrate, duration_ms);
            }
        }
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);

    // Calculate expected hashrate WITH percentage target built-in
    uint8_t asic_count = ASIC_get_asic_count(GLOBAL_STATE);
    uint16_t small_core_count = ASIC_get_small_core_count(GLOBAL_STATE);
    float frequency_mhz = GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value;
    float hashrate_test_percentage_target = 0.4f;  // 40% for BM1370
    
    float expected_hashrate_ghs = frequency_mhz
                                * small_core_count
                                * hashrate_test_percentage_target
                                * asic_count
                                / 1000.0f;

    ESP_LOGI(TAG, "Hashrate: %.2f, Expected: %.2f", hashrate, expected_hashrate_ghs);

    if (hashrate < expected_hashrate_ghs) {
        ESP_LOGE(TAG, "Hashrate test failed!");
        tests_done(GLOBAL_STATE, TESTS_FAILED, ASIC_FAILURE);
    }

    free(GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs);
    free(GLOBAL_STATE->valid_jobs);

    if (test_core_voltage(GLOBAL_STATE) != ESP_OK) {
        tests_done(GLOBAL_STATE, TESTS_FAILED, POWER_FAILURE);
    }

    switch (GLOBAL_STATE->device_model) {
        case BITFORGE_NANO:
            if (test_INA260_power_consumption(POWER_CONSUMPTION_NANO, POWER_CONSUMPTION_MARGIN) != ESP_OK) {
                ESP_LOGE(TAG, "INA260 Power Draw Failed, target %.2f", (float)POWER_CONSUMPTION_NANO);
                tests_done(GLOBAL_STATE, TESTS_FAILED, POWER_FAILURE);
            }
            break;
        default:
    }
    tests_done(GLOBAL_STATE, TESTS_PASSED, NO_FAILURE);

    return;  
}

static void tests_done(GlobalState * GLOBAL_STATE, bool test_result, TEST_FAILED_CAUSE cause) 
{

    Power_disable(GLOBAL_STATE);

    if (test_result == TESTS_FAILED) {
        ESP_LOGI(TAG, "SELF TESTS FAIL -- Press RESET to continue");  

        switch(cause)
        {
            case NO_FAILURE:
                break;
            case ASIC_FAILURE:
                switch_led(2, 1);
                break;
            case POWER_FAILURE:
                switch_led(1, 1);
                switch_led(2, 1);
                break;
            case PERIPHERAL_FAILURE:
                switch_led(1, 1);
                break;
            default:
                break;

        }

        while (1) {
            // Wait here forever until reset_self_test() gives the BootSemaphore
            if (xSemaphoreTake(BootSemaphore, portMAX_DELAY) == pdTRUE) {
                nvs_config_set_u16(NVS_CONFIG_PRODUCTION_TEST, 0);
                //wait 100ms for nvs write to finish?
                vTaskDelay(100 / portTICK_PERIOD_MS);
                esp_restart();
            }
        }
    } else {
        nvs_config_set_u16(NVS_CONFIG_PRODUCTION_TEST, 0);
        ESP_LOGI(TAG, "Self Tests Passed!!!");

        while (1) {
            switch_led(1, 1);
            switch_led(2, 1);
            vTaskDelay(500 / portTICK_PERIOD_MS);
            
            switch_led(1, 0);
            switch_led(2, 0);
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
    }

}
