#include <stdio.h>
#include "esp_log.h"

#include "i2c_bitforge.h"
#include "EMC2103.h"

static const char * TAG = "EMC2103";

static i2c_master_dev_handle_t EMC2103_dev_handle;

/**
 * @brief Initialize the EMC2103 sensor.
 *
 * @return esp_err_t ESP_OK on success, or an error code on failure.
 */
esp_err_t EMC2103_init(bool invertPolarity) {

    if (i2c_bitforge_add_device(EMC2103_I2CADDR_DEFAULT, &EMC2103_dev_handle, TAG) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add device");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "EMC2103 init with polarity %d", invertPolarity);

    // Configure the fan setting
    esp_err_t err = i2c_bitforge_register_write_byte(EMC2103_dev_handle, EMC2103_CONFIGURATION1, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write CONFIGURATION1: %s", esp_err_to_name(err));
        return err;
    }

    if (invertPolarity) {
        err = i2c_bitforge_register_write_byte(EMC2103_dev_handle, EMC2103_PWM_CONFIG, 0x01);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write PWM_CONFIG: %s", esp_err_to_name(err));
            return err;
        }
    }

    return ESP_OK;

}

void EMC2103_set_ideality_factor(uint8_t ideality){
    //set Ideality Factor
    esp_err_t err = i2c_bitforge_register_write_byte(EMC2103_dev_handle, EMC2103_EXTERNAL_DIODE1_IDEALITY, ideality);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set diode 1 ideality factor: %s", esp_err_to_name(err));
    }
    err = i2c_bitforge_register_write_byte(EMC2103_dev_handle, EMC2103_EXTERNAL_DIODE2_IDEALITY, ideality);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set diode 2 ideality factor: %s", esp_err_to_name(err));
    }
}

void EMC2103_set_beta_compensation(uint8_t beta){
    //set Beta Compensation
    esp_err_t err = i2c_bitforge_register_write_byte(EMC2103_dev_handle, EMC2103_EXTERNAL_DIODE1_BETA, beta);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set diode 1 beta compensation: %s", esp_err_to_name(err));
    }
    err = i2c_bitforge_register_write_byte(EMC2103_dev_handle, EMC2103_EXTERNAL_DIODE2_BETA, beta);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set diode 2 beta compensation: %s", esp_err_to_name(err));
    }
}

/**
 * @brief Set the fan speed as a percentage.
 *
 * @param percent The desired fan speed as a percentage (0.0 to 1.0).
 */
void EMC2103_set_fan_speed(float percent)
{
    uint8_t setting = (uint8_t) (255.0 * percent);
    ESP_LOGI(TAG, "Setting fan speed to %.2f%% (%d)", percent*100.0, setting);
    esp_err_t err = i2c_bitforge_register_write_byte(EMC2103_dev_handle, EMC2103_FAN_SETTING, setting);
    if (err != ESP_OK) {
        // A transient NACK here must not be fatal: the fan keeps its previous
        // setting and the next control-loop iteration retries.
        ESP_LOGE(TAG, "Failed to set fan speed: %s", esp_err_to_name(err));
    }
}

/**
 * @brief Get the current fan speed in RPM.
 *
 * @return uint16_t The fan speed in RPM.
 */
uint16_t EMC2103_get_fan_speed(void)
{
    uint8_t tach_lsb = 0, tach_msb = 0;
    uint16_t reading;
    uint32_t RPM;
    esp_err_t err;

    err = i2c_bitforge_register_read(EMC2103_dev_handle, EMC2103_TACH_LSB, &tach_lsb, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read tach LSB: %s", esp_err_to_name(err));
        return 0;
    }

    err = i2c_bitforge_register_read(EMC2103_dev_handle, EMC2103_TACH_MSB, &tach_msb, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read tach MSB: %s", esp_err_to_name(err));
        return 0;
    }

    ESP_LOGI(TAG, "Raw Fan Speed = %02X %02X", tach_msb, tach_lsb);

    reading = tach_lsb | (tach_msb << 8);
    reading >>= 3;

    if (reading == 0) {
        // A zero tach reading would trap on the divide below.
        ESP_LOGW(TAG, "Invalid tach reading of 0");
        return 0;
    }

    //RPM = (3,932,160 * m)/reading
    //m is the multipler, which is default 2
    RPM = 7864320 / reading;

    // ESP_LOGI(TAG, "Fan Speed = %d RPM", RPM);
    if (RPM == 82) {
        return 0;
    }
    return RPM;
}

/**
 * @brief Get the external temperature in Celsius.
 *
 * @return float The external temperature in Celsius.
 */
float EMC2103_get_external_temp(void)
{
    uint8_t temp_msb = 0, temp_lsb = 0;
    uint16_t reading;
    esp_err_t err;

    float temp1, temp2;

    err = i2c_bitforge_register_read(EMC2103_dev_handle, EMC2103_EXTERNAL_TEMP1_MSB, &temp_msb, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read external temperature 1 MSB: %s", esp_err_to_name(err));
        return 0.0f;
    }

    err = i2c_bitforge_register_read(EMC2103_dev_handle, EMC2103_EXTERNAL_TEMP1_LSB, &temp_lsb, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read external temperature 1 LSB: %s", esp_err_to_name(err));
        return 0.0f;
    }

    //print the temps
    //ESP_LOGI(TAG, "Temp1 MSB: %02X Temp1 LSB: %02X", temp_msb, temp_lsb);
    
    // Combine MSB and LSB, and then right shift to get 11 bits
    reading = (temp_msb << 8) | temp_lsb;

    if (reading == EMC2103_TEMP_DIODE_FAULT) {
        ESP_LOGE(TAG, "EMC2103 TEMP_DIODE1_FAULT: %04X", reading);
    }

    reading >>= 5;  // Now, `reading` contains an 11-bit signed value

    // Cast `reading` to a signed 16-bit integer
    int16_t signed_reading = (int16_t)reading;

    // If the 11th bit (sign bit in 11-bit data) is set, extend the sign
    if (signed_reading & 0x0400) {
        signed_reading |= 0xF800;  // Set upper bits to extend the sign
    }

    // Convert the signed reading to temperature in Celsius
    temp1 = (float)signed_reading / 8.0;

    // temp1 is the returned value, so a failure to read diode 2 is only
    // logged - it costs us the debug print below, not the measurement.
    err = i2c_bitforge_register_read(EMC2103_dev_handle, EMC2103_EXTERNAL_TEMP2_MSB, &temp_msb, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read external temperature 2 MSB: %s", esp_err_to_name(err));
        return temp1;
    }

    err = i2c_bitforge_register_read(EMC2103_dev_handle, EMC2103_EXTERNAL_TEMP2_LSB, &temp_lsb, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read external temperature 2 LSB: %s", esp_err_to_name(err));
        return temp1;
    }

    //print the temps
    //ESP_LOGI(TAG, "Temp2 MSB: %02X Temp2 LSB: %02X", temp_msb, temp_lsb);
    
    // Combine MSB and LSB, and then right shift to get 11 bits
    reading = (temp_msb << 8) | temp_lsb;
    if (reading == EMC2103_TEMP_DIODE_FAULT) {
        ESP_LOGE(TAG, "EMC2103 TEMP_DIODE2_FAULT: %04X", reading);
    }
    reading >>= 5;  // Now, `reading` contains an 11-bit signed value

    // Cast `reading` to a signed 16-bit integer
    signed_reading = (int16_t)reading;

    // If the 11th bit (sign bit in 11-bit data) is set, extend the sign
    if (signed_reading & 0x0400) {
        signed_reading |= 0xF800;  // Set upper bits to extend the sign
    }

    // Convert the signed reading to temperature in Celsius
    temp2 = (float)signed_reading / 8.0;


    //debug the temps
    ESP_LOGI(TAG, "Temp1: %.2f Temp2: %.2f", temp1, temp2);

    return temp1;
}
