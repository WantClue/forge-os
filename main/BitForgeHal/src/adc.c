#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "adc.h"
#include <math.h>

#define ADC_ATTEN_CHANNEL_1  ADC_ATTEN_DB_12
#define ADC_ATTEN_CHANNEL_3  ADC_ATTEN_DB_12
#define ADC_ATTEN_CHANNEL_4  ADC_ATTEN_DB_12
#define ADC_ATTEN_CHANNEL_5  ADC_ATTEN_DB_12
#define ADC_ATTEN_CHANNEL_6  ADC_ATTEN_DB_12
#define ADC_CHANNEL_0 ADC_CHANNEL_0
#define ADC_CHANNEL_1 ADC_CHANNEL_1
#define ADC_CHANNEL_2 ADC_CHANNEL_2
#define ADC_CHANNEL_3 ADC_CHANNEL_3
#define ADC_CHANNEL_4 ADC_CHANNEL_4
#define ADC_CHANNEL_5 ADC_CHANNEL_5
#define ADC_CHANNEL_6 ADC_CHANNEL_6
#define ADC_CHANNEL_7 ADC_CHANNEL_7
#define ADC_CHANNEL_8 ADC_CHANNEL_8
#define ADC_CHANNEL_9 ADC_CHANNEL_9
#define BETA 3380  // Beta value of the thermistor
#define R0 10000   // Resistance at 25°C (10kΩ)

static const char * TAG = "ADC";

static adc_cali_handle_t adc1_cali_chan1_handle;
static adc_cali_handle_t adc1_cali_chan3_handle;
static adc_cali_handle_t adc1_cali_chan4_handle;
static adc_cali_handle_t adc1_cali_chan5_handle;
static adc_cali_handle_t adc1_cali_chan6_handle;
static adc_oneshot_unit_handle_t adc1_handle;


/*---------------------------------------------------------------
        ADC Calibration
---------------------------------------------------------------*/
static bool adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated) {
        ESP_LOGI(TAG, "calibration scheme version is %s", "Curve Fitting");
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit,
            .chan = channel,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
        if (ret == ESP_OK) {
            calibrated = true;
        }
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        ESP_LOGI(TAG, "calibration scheme version is %s", "Line Fitting");
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
        if (ret == ESP_OK) {
            calibrated = true;
        }
    }
#endif

    *out_handle = handle;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Calibration Success");
    } else if (ret == ESP_ERR_NOT_SUPPORTED || !calibrated) {
        ESP_LOGW(TAG, "eFuse not burnt, skip software calibration");
    } else {
        ESP_LOGE(TAG, "Invalid arg or no memory");
    }

    return calibrated;
}

// Sets up the ADC to read Vcore. Run this before ADC_get_vcore()
void ADC_init(GlobalState * GLOBAL_STATE)
{
    //-------------ADC1 Init---------------//
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    //-------------ADC1 Channel 1 Config---------------//
    adc_oneshot_chan_cfg_t config_channel_1 = {
        .atten = ADC_ATTEN_CHANNEL_1,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_1, &config_channel_1)); // VDD PowerSupply ASICs

    switch (GLOBAL_STATE->device_model) {
        case BITFORGE_NANO:
            //-------------ADC1 Channel 3 Config---------------//
            adc_oneshot_chan_cfg_t config_channel_3 = {
                .atten = ADC_ATTEN_CHANNEL_3,
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            };

            //-------------ADC1 Channel 4 Config---------------//
            adc_oneshot_chan_cfg_t config_channel_4 = {
                .atten = ADC_ATTEN_CHANNEL_4,
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            };

            //-------------ADC1 Channel 5 Config---------------//
            adc_oneshot_chan_cfg_t config_channel_5 = {
                .atten = ADC_ATTEN_CHANNEL_5,
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            };

            //-------------ADC1 Channel 6 Config---------------//
            adc_oneshot_chan_cfg_t config_channel_6 = {
                .atten = ADC_ATTEN_CHANNEL_6,
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            };

            ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_3, &config_channel_3)); // Thermistor 10k A2
            ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_4, &config_channel_4)); // Thermistor 10k A1
            ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_5, &config_channel_5)); // 0V8 reference voltage ASIC
            ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_6, &config_channel_6)); // 1V2 reference voltage ASIC
        default:
    }

    //-------------ADC1 Channel 1 Calibration Init---------------//
    adc_calibration_init(ADC_UNIT_1, ADC_CHANNEL_1, ADC_ATTEN_CHANNEL_1, &adc1_cali_chan1_handle);

    switch (GLOBAL_STATE->device_model) {
        case BITFORGE_NANO:
            //-------------ADC1 Channel 3 Calibration Init---------------//
            adc_calibration_init(ADC_UNIT_1, ADC_CHANNEL_3, ADC_ATTEN_CHANNEL_3, &adc1_cali_chan3_handle);
            //-------------ADC1 Channel 4 Calibration Init---------------//
            adc_calibration_init(ADC_UNIT_1, ADC_CHANNEL_4, ADC_ATTEN_CHANNEL_4, &adc1_cali_chan4_handle);
            //-------------ADC1 Channel 5 Calibration Init---------------//
            adc_calibration_init(ADC_UNIT_1, ADC_CHANNEL_5, ADC_ATTEN_CHANNEL_5, &adc1_cali_chan5_handle);
            //-------------ADC1 Channel 6 Calibration Init---------------//
            adc_calibration_init(ADC_UNIT_1, ADC_CHANNEL_6, ADC_ATTEN_CHANNEL_6, &adc1_cali_chan6_handle);
        default:
    }

}

// returns the ADC voltage in mV
uint16_t ADC_get_vcore(void)
{
    int adc_raw[2][10];
    int voltage[2][10];

    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_1, &adc_raw[0][1]));

    ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan1_handle, adc_raw[0][1], &voltage[0][1]));

    return (uint16_t)voltage[0][1];
}

// returns the ADC of a specified channel
uint16_t ADC_read(ADC_CHANNEL Channel, GlobalState * GLOBAL_STATE)
{
    int adc_raw[2][10];
    int voltage[2][10];

    switch (GLOBAL_STATE->device_model) {
        case BITFORGE_NANO:
            switch(Channel) {
                case V_BUCK_OUTPUT:
                    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_1, &adc_raw[0][1]));
                    ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan1_handle, adc_raw[0][1], &voltage[0][1]));
                    return (uint16_t)voltage[0][1];
                case V_TEMP_10K_A2:
                    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_3, &adc_raw[0][1]));
                    ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan3_handle, adc_raw[0][1], &voltage[0][1]));
                    return (uint16_t)voltage[0][1];
                case V_TEMP_10K_A1:
                    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_4, &adc_raw[0][1]));
                    ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan4_handle, adc_raw[0][1], &voltage[0][1]));
                    return (uint16_t)voltage[0][1];
                case V_0V8_REF:
                    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_5, &adc_raw[0][1]));
                    ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan5_handle, adc_raw[0][1], &voltage[0][1]));
                    return (uint16_t)voltage[0][1];
                case V_1V2_REF:
                    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_6, &adc_raw[0][1]));
                    ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan6_handle, adc_raw[0][1], &voltage[0][1]));
                    return (uint16_t)voltage[0][1];
                default:
            }
        default:
    }
    return 0U;
}

float ADC_get_temperature(ADC_CHANNEL Channel, DeviceModel Device) {
    int adc_raw[2][10];
    int voltage_raw[2][10];

    switch (Device) {
        case BITFORGE_NANO:
            switch(Channel) {
                case V_TEMP_10K_A2:
                    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_3, &adc_raw[0][1]));
                    ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan3_handle, adc_raw[0][1], &voltage_raw[0][1]));
                    break;
                case V_TEMP_10K_A1:
                    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_4, &adc_raw[0][1]));
                    ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan4_handle, adc_raw[0][1], &voltage_raw[0][1]));
                    break;
                default:
                }
        default:
    }
   
    // Convert millivolts to volts
    float voltage = voltage_raw[0][1] / 1000.0;

    // Ensure the voltage is within a valid range
    if (voltage <= 0) {
        printf("Error: Invalid voltage reading.\n");
        return -273.15; // Return a clearly invalid temperature to indicate an error
    }

    // Calculate the thermistor resistance using the voltage divider formula
    // R_T = R0 * (Vout / (VREF - Vout))
    float thermistor_resistance = R0 * (voltage / (3.3 - voltage));

    // Use the Beta parameter equation to calculate the temperature in Kelvin
    float temperature_kelvin = (float)(BETA / (log(thermistor_resistance / R0) + (BETA / 298.15)));

    // Convert the temperature to Celsius
    float temperature_celsius = temperature_kelvin - 273.15;

    return temperature_celsius + 11U;
}