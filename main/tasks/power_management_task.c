#include <string.h>
#include "INA260.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "global_state.h"
#include "math.h"
#include "mining.h"
#include "nvs_config.h"
#include "serial.h"
#include "TPS546.h"
#include "vcore.h"
#include "PAC9544.h"
#include "ThermalMonitoring.h"
#include "power.h"
#include "asic.h"
#include "adc.h"
#include "hashrate_monitor_task.h"

#define POLL_RATE 2000
#define MAX_TEMP 90.0
#define THROTTLE_TEMP 75.0
#define THROTTLE_TEMP_RANGE (MAX_TEMP - THROTTLE_TEMP)

#define VOLTAGE_START_THROTTLE 4900
#define VOLTAGE_MIN_THROTTLE 3500
#define VOLTAGE_RANGE (VOLTAGE_START_THROTTLE - VOLTAGE_MIN_THROTTLE)

#define TPS546_THROTTLE_TEMP 105.0
#define TPS546_MAX_TEMP 145.0
#define PAUSED_FAN_SPEED 30.0

static const char * TAG = "power_management";

static bool even = false;

static void update_input_telemetry(GlobalState * GLOBAL_STATE)
{
    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;

    power_management->voltage = Power_get_input_voltage(GLOBAL_STATE);
    power_management->current = Power_get_current(GLOBAL_STATE);
    power_management->power = Power_get_power(GLOBAL_STATE);
}

static void update_paused_telemetry(GlobalState * GLOBAL_STATE)
{
    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;

    update_input_telemetry(GLOBAL_STATE);

    PAC9544_selectChannel(even + 2U);
    vTaskDelay(pdMS_TO_TICKS(10));
    power_management->vr_temp = Power_get_vreg_temp(GLOBAL_STATE);
    power_management->fan_rpm[even] = Thermal_getFanSpeed();

    // ASIC thermal diodes are not valid while VCORE is disabled.
    power_management->chip_temp[0] = 0.0f;
    power_management->chip_temp[1] = 0.0f;
    power_management->chip_temp_avg = 0.0f;

    even = !even;
}

static void mining_stop(GlobalState * GLOBAL_STATE)
{
    ESP_LOGI(TAG, "Stopping mining");

    // Stop the worker tasks before changing the ASIC clock and power state.
    GLOBAL_STATE->ASIC_initalized = false;
    hashrate_monitor_reset(GLOBAL_STATE);

    // Match AxeOS: wind the chain down before switching off the TPS546 output.
    if (!ASIC_set_frequency(GLOBAL_STATE, 50.0f)) {
        ESP_LOGW(TAG, "Unable to reduce ASIC frequency before pause");
    }

    if (VCORE_set_voltage(0.0f, GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Unable to turn off ASIC core voltage");
    }
    ASIC_hold_reset_low(GLOBAL_STATE);

    // Let in-flight UART work finish, then discard stale bytes.
    vTaskDelay(500 / portTICK_PERIOD_MS);
    SERIAL_clear_buffer();
    vTaskDelay(100 / portTICK_PERIOD_MS);

    hashrate_monitor_reset(GLOBAL_STATE);
    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.fan_perc = PAUSED_FAN_SPEED;
    Thermal_setFanSpeedPercent(PAUSED_FAN_SPEED / 100.0f);

    ESP_LOGI(TAG, "Mining stopped");
}

static bool mining_start(GlobalState * GLOBAL_STATE)
{
    ESP_LOGI(TAG, "Starting mining");

    uint16_t voltage = nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE, CONFIG_ASIC_VOLTAGE);
    if (VCORE_set_voltage((double) voltage / 1000.0, GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Unable to restore ASIC core voltage");
        return false;
    }

    vTaskDelay(500 / portTICK_PERIOD_MS);

    // A freshly reset BM1370 chain starts at the default UART baud.
    SERIAL_set_baud(115200);
    SERIAL_clear_buffer();
    vTaskDelay(100 / portTICK_PERIOD_MS);

    uint8_t chip_count = ASIC_init(GLOBAL_STATE);
    if (chip_count == 0) {
        ESP_LOGE(TAG, "Mining resume failed: ASIC chain not detected");
        ASIC_hold_reset_low(GLOBAL_STATE);
        VCORE_set_voltage(0.0f, GLOBAL_STATE);
        return false;
    }

    SERIAL_set_baud(ASIC_set_max_baud(GLOBAL_STATE));
    SERIAL_clear_buffer();

    // ASIC counters restart with the chain. Discard every pre-pause baseline
    // before register polling is allowed to resume.
    hashrate_monitor_reset(GLOBAL_STATE);
    GLOBAL_STATE->ASIC_initalized = true;

    ESP_LOGI(TAG, "Mining started successfully (%u chip(s))", chip_count);
    return true;
}

// Set the fan speed between 35% min and 100% max based on ASIC and VR temperatures.
// Uses the higher of the two temperature-based fan speed requirements.
// ASIC: 45°C (35%) → 75°C (100%)
// VR: 65°C (35%) → 90°C (100%)
static double automatic_fan_speed(float chip_temp, float vr_temp, GlobalState * GLOBAL_STATE)
{
    double min_fan_speed = 35.0;
    
    // Calculate fan speed based on ASIC temperature
    double asic_min_temp = 45.0;
    double asic_max_temp = THROTTLE_TEMP; // 75.0°C
    double asic_fan_speed = min_fan_speed;
    
    if (chip_temp < asic_min_temp) {
        asic_fan_speed = min_fan_speed;
    } else if (chip_temp >= asic_max_temp) {
        asic_fan_speed = 100.0;
    } else {
        double asic_temp_range = asic_max_temp - asic_min_temp;
        double fan_range = 100.0 - min_fan_speed;
        asic_fan_speed = ((chip_temp - asic_min_temp) / asic_temp_range) * fan_range + min_fan_speed;
    }
    
    // Calculate fan speed based on VR temperature
    double vr_min_temp = 60.0;
    double vr_max_temp = 85.0;
    double vr_fan_speed = min_fan_speed;
    
    if (vr_temp < vr_min_temp) {
        vr_fan_speed = min_fan_speed;
    } else if (vr_temp >= vr_max_temp) {
        vr_fan_speed = 100.0;
    } else {
        double vr_temp_range = vr_max_temp - vr_min_temp;
        double fan_range = 100.0 - min_fan_speed;
        vr_fan_speed = ((vr_temp - vr_min_temp) / vr_temp_range) * fan_range + min_fan_speed;
    }
    
    // Use the higher of the two calculated fan speeds
    double result = (asic_fan_speed > vr_fan_speed) ? asic_fan_speed : vr_fan_speed;
    
    #ifdef POWER_DEBUG
    const char* driver = (asic_fan_speed > vr_fan_speed) ? "ASIC" : "VR";
    ESP_LOGI(TAG, "Auto Fan: ASIC=%.1f°C(%.1f%%) VR=%.1f°C(%.1f%%) -> %.1f%% [%s]",
             chip_temp, asic_fan_speed, vr_temp, vr_fan_speed, result, driver);
    #endif
    
    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;
    power_management->fan_perc = result;
    Thermal_setFanSpeedPercent(result/100.0);
    return result;
}

void POWER_MANAGEMENT_task(void * pvParameters)
{
    ESP_LOGI(TAG, "Starting");

    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;
    SystemModule * sys_module = &GLOBAL_STATE->SYSTEM_MODULE;

    power_management->frequency_multiplier = 1;

    //int last_frequency_increase = 0;
    //uint16_t frequency_target = nvs_config_get_u16(NVS_CONFIG_ASIC_FREQ, CONFIG_ASIC_FREQUENCY);

    vTaskDelay(500 / portTICK_PERIOD_MS);
    uint16_t last_core_voltage = 0.0;
    uint16_t last_asic_frequency = power_management->frequency_value;
    bool is_user_paused = false;
    
    while (1) {
        if (sys_module->mining_paused && !is_user_paused) {
            sys_module->mining_state = MINING_STATE_PAUSING;
            mining_stop(GLOBAL_STATE);
            is_user_paused = true;
            if (!sys_module->mining_paused) {
                sys_module->mining_state = MINING_STATE_RESUMING;
                continue;
            }
            sys_module->mining_state = MINING_STATE_PAUSED;
        } else if (!sys_module->mining_paused && is_user_paused) {
            sys_module->mining_state = MINING_STATE_RESUMING;
            if (mining_start(GLOBAL_STATE)) {
                is_user_paused = false;
                if (sys_module->mining_paused) {
                    sys_module->mining_state = MINING_STATE_PAUSING;
                    continue;
                }
                sys_module->mining_state = MINING_STATE_MINING;
            } else {
                // Keep the externally visible state honest if recovery fails.
                sys_module->mining_paused = true;
                sys_module->mining_state = MINING_STATE_PAUSED;
            }
        }

        if (is_user_paused) {
            // Keep board-level telemetry live while the ASIC rail is off.
            update_paused_telemetry(GLOBAL_STATE);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_RATE));
            continue;
        }

        PAC9544_selectChannel(even + 2U);
        vTaskDelay(pdMS_TO_TICKS(10)); // Allow PAC9544 channel switch to settle

        update_input_telemetry(GLOBAL_STATE);
        #ifdef POWER_DEBUG
        ESP_LOGI(TAG, "POWER: %f", power_management->power);
        #endif
        power_management->vr_temp = Power_get_vreg_temp(GLOBAL_STATE);
        int16_t voltage = VCORE_get_voltage_mv(GLOBAL_STATE);
        #ifdef POWER_DEBUG
        ESP_LOGI(TAG, "VCORE: %d", voltage);
        #endif

        power_management->fan_rpm[even] = Thermal_getFanSpeed();

        PAC9544_selectChannel(2);
        vTaskDelay(pdMS_TO_TICKS(10)); // Allow PAC9544 channel switch to settle
        float temp_ASIC_1 = Thermal_getAsicChipTemp(GLOBAL_STATE);
        #ifdef POWER_DEBUG
        ESP_LOGI(TAG, "External Temp of EMC2101_ASIC1: %f", temp_ASIC_1);
        #endif
        PAC9544_selectChannel(3);
        vTaskDelay(pdMS_TO_TICKS(10)); // Allow PAC9544 channel switch to settle
        float temp_ASIC_2 = Thermal_getAsicChipTemp(GLOBAL_STATE);
        #ifdef POWER_DEBUG
        ESP_LOGI(TAG, "External Temp of EMC2101_ASIC2: %f", temp_ASIC_2);
        #endif

        power_management->chip_temp_avg = (temp_ASIC_1 + temp_ASIC_2 )/2;

        // Store individual ASIC temperatures in the array
        power_management->chip_temp[0] = temp_ASIC_1;
        power_management->chip_temp[1] = temp_ASIC_2;

        // ASIC Thermal Diode will give bad readings if the ASIC is turned off
        // if(power_management->voltage < tps546_config.TPS546_INIT_VOUT_MIN){
        //     goto looper;
        // }

        //overheat mode if the voltage regulator or ASICs are too hot
        // Only trigger overheat if temperatures are valid (> 0) and actually high
        bool asic1_overheat = (power_management->chip_temp[0] > 0.0f && power_management->chip_temp[0] > THROTTLE_TEMP);
        bool asic2_overheat = (power_management->chip_temp[1] > 0.0f && power_management->chip_temp[1] > THROTTLE_TEMP);
        bool vr_overheat = (power_management->vr_temp > TPS546_THROTTLE_TEMP);
        
        if ((vr_overheat || asic1_overheat || asic2_overheat) && (power_management->frequency_value > 50 || power_management->voltage > 1000)) {
            ESP_LOGE(TAG, "OVERHEAT! VR: %fC ASIC1: %fC ASIC2: %fC", power_management->vr_temp, power_management->chip_temp[0], power_management->chip_temp[1]);
            power_management->fan_perc = 100;
            Thermal_setFanSpeedPercent(1);

            // Turn off core voltage
            Power_disable(GLOBAL_STATE);

            nvs_config_set_u16(NVS_CONFIG_ASIC_VOLTAGE, 1000);
            nvs_config_set_u16(NVS_CONFIG_ASIC_FREQ, 50);
            nvs_config_set_u16(NVS_CONFIG_FAN_SPEED, 100);
            nvs_config_set_u16(NVS_CONFIG_AUTO_FAN_SPEED, 0);
            nvs_config_set_u16(NVS_CONFIG_OVERHEAT_MODE, 1);
            exit(EXIT_FAILURE);
        }

        if (nvs_config_get_u16(NVS_CONFIG_AUTO_FAN_SPEED, 1) == 1) {
            // Use the higher of the two valid ASIC temperatures for fan control
            float temp_for_fan = 0.0f;
            if (power_management->chip_temp[0] > 0.0f && power_management->chip_temp[1] > 0.0f) {
                temp_for_fan = (power_management->chip_temp[0] > power_management->chip_temp[1]) ? 
                               power_management->chip_temp[0] : power_management->chip_temp[1];
            } else if (power_management->chip_temp[0] > 0.0f) {
                temp_for_fan = power_management->chip_temp[0];
            } else if (power_management->chip_temp[1] > 0.0f) {
                temp_for_fan = power_management->chip_temp[1];
            } else {
                // No valid temperatures, use a conservative temperature for fan control
                temp_for_fan = 50.0f;
            }

            power_management->fan_perc = (float)automatic_fan_speed(temp_for_fan, power_management->vr_temp, GLOBAL_STATE);

        } else {
            float fs = (float) nvs_config_get_u16(NVS_CONFIG_FAN_SPEED, 100);
            power_management->fan_perc = fs;
            Thermal_setFanSpeedPercent((float) fs / 100.0);
        }

        // New voltage and frequency adjustment code
        uint16_t core_voltage = nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE, CONFIG_ASIC_VOLTAGE);
        uint16_t asic_frequency = nvs_config_get_u16(NVS_CONFIG_ASIC_FREQ, CONFIG_ASIC_FREQUENCY);

        if (core_voltage != last_core_voltage) {
            ESP_LOGI(TAG, "setting new vcore voltage to %umV", core_voltage);
            VCORE_set_voltage((double) core_voltage / 1000.0, GLOBAL_STATE);
            last_core_voltage = core_voltage;
        }

        if (asic_frequency != last_asic_frequency) {
            ESP_LOGI(TAG, "New ASIC frequency requested: %uMHz (current: %uMHz)", asic_frequency, last_asic_frequency);
            
            bool success = ASIC_set_frequency(GLOBAL_STATE, (float)asic_frequency);
            
            if (success) {
                power_management->frequency_value = (float)asic_frequency;
            }
            
            last_asic_frequency = asic_frequency;
        }

        // Check for changing of overheat mode
        uint16_t new_overheat_mode = nvs_config_get_u16(NVS_CONFIG_OVERHEAT_MODE, 0);
        
        if (new_overheat_mode != sys_module->overheat_mode) {
            sys_module->overheat_mode = new_overheat_mode;
            ESP_LOGI(TAG, "Overheat mode updated to: %d", sys_module->overheat_mode);
        }

        VCORE_check_fault(GLOBAL_STATE);
        even = !even;
        // API pause/resume requests wake this task without shortening sensor polling.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_RATE));
    }
}
