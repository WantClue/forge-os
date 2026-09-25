#include "ThermalMonitoring.h"
#include "adc.h"
#include "PAC9544.h"
#include "EMC2101.h"

#define INTERNAL_OFFSET 5 //degrees C

/* Globals */
static DeviceModel _deviceModel;
static const char *TAG = "ThermalMonitoring";


esp_err_t Thermal_init(GlobalState * GLOBAL_STATE) {

    esp_err_t result = ESP_OK;
    _deviceModel = GLOBAL_STATE->device_model;
    float temp = 0U;

    switch (_deviceModel) {
        case BITFORGE_NANO:
            PAC9544_init();
            
            // First EMC2101
            PAC9544_selectChannel(2);
            vTaskDelay(pdMS_TO_TICKS(10)); // Allow PAC9544 channel switch to settle
            EMC2101_init();
            EMC2101_setIdealityFactor(EMC2101_IDEALITY_1_0566);
            EMC2101_setBetaCompensation(EMC2101_BETA_11);
            // Initial fan speed will be set by power management task
            float temp_ASIC_1 = Thermal_getAsicChipTemp(GLOBAL_STATE);
            #ifdef DEBUG_THERMALMONITORING
            ESP_LOGI(TAG, "External Temp of EMC2101_ASIC1: %f", temp_ASIC_1);
            #endif
            temp_ASIC_1 = ADC_get_temperature(V_TEMP_10K_A1, _deviceModel);
            #ifdef DEBUG_THERMALMONITORING
            ESP_LOGI(TAG, "External Thermistor Temp of ASIC1: %f", temp_ASIC_1);
            #endif

            // Second EMC2101
            PAC9544_selectChannel(3);
            vTaskDelay(pdMS_TO_TICKS(10)); // Allow PAC9544 channel switch to settle
            EMC2101_init();
            EMC2101_setIdealityFactor(EMC2101_IDEALITY_1_0566);
            EMC2101_setBetaCompensation(EMC2101_BETA_11);
            // Initial fan speed will be set by power management task
            float temp_ASIC_2 = Thermal_getAsicChipTemp(GLOBAL_STATE);
            #ifdef DEBUG_THERMALMONITORING
            ESP_LOGI(TAG, "External Temp of EMC2101_ASIC2: %f", temp_ASIC_2);
            #endif
            temp_ASIC_2 = ADC_get_temperature(V_TEMP_10K_A2, _deviceModel);
            #ifdef DEBUG_THERMALMONITORING
            ESP_LOGI(TAG, "External Thermistor Temp of ASIC2: %f", temp_ASIC_2);
            #endif
            break;
        default:
            result = ESP_FAIL;
    }
    return result;
}

//percent is a float between 0.0 and 1.0
esp_err_t Thermal_setFanSpeedPercent(float percent)
{
    switch (_deviceModel) {
        case BITFORGE_NANO:
            // Set fan speed on first EMC2101 (channel 2)
            PAC9544_selectChannel(2);
            vTaskDelay(pdMS_TO_TICKS(10)); // Allow PAC9544 channel switch to settle
            EMC2101_setFanSpeed(percent);
            
            // Set fan speed on second EMC2101 (channel 3)
            PAC9544_selectChannel(3);
            vTaskDelay(pdMS_TO_TICKS(10)); // Allow PAC9544 channel switch to settle
            EMC2101_setFanSpeed(percent);
            break;
        default:
            return ESP_FAIL;
    }
    return ESP_OK;
}

uint16_t Thermal_getFanSpeed(void)
{
    switch (_deviceModel) {
        case BITFORGE_NANO:
            // Read fan speed from first EMC2101 (channel 2)
            PAC9544_selectChannel(2);
            vTaskDelay(pdMS_TO_TICKS(10)); // Allow PAC9544 channel switch to settle
            return EMC2101_getFanSpeed();
        default:
            return 0;
    }
}

float Thermal_getAsicChipTemp(GlobalState * GLOBAL_STATE)
{
    if (!GLOBAL_STATE->ASIC_initalized) {
        return -1;
    }
    
    switch (_deviceModel) {
        case BITFORGE_NANO:
            return EMC2101_getExternalTemp();
        default:
            return -1;
    }
}