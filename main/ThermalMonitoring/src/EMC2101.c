#include "EMC2101.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char * TAG = "EMC2101";

static i2c_master_dev_handle_t EMC2101_dev_handle;
static i2c_master_bus_handle_t i2c_bus_handle;

/**
 * @brief Initialize the EMC2101 sensor.
 *
 * @return esp_err_t ESP_OK on success, or an error code on failure.
 */
esp_err_t EMC2101_init() {
    /*if (i2c_bitforge_add_device(EMC2101_I2CADDR_DEFAULT, &EMC2101_dev_handle, TAG) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add device");
        return ESP_FAIL;
    }*/
    esp_err_t ret = ESP_OK;

    i2c_device_config_t EMC2101_dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = EMC2101_I2C_ADDR,
        .scl_speed_hz = I2C_BUS_SPEED_HZ,
    };

    // Get the I2C bus handle
    ret = i2c_bitforge_get_master_bus_handle(&i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get I2C bus handle: %s", esp_err_to_name(ret));
        return ret;
    }

    // Add a longer delay to allow the I2C bus to stabilize
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ret = i2c_master_bus_add_device(i2c_bus_handle, &EMC2101_dev_cfg, &EMC2101_dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init: Failed to add device");
        return ret;
    }

    if (ret == ESP_OK) {
        ret = i2c_bitforge_register_write_byte(EMC2101_dev_handle, EMC2101_REG_CONFIG, 0x04);
        if (ret != ESP_OK) {
            ESP_LOGI(TAG, "EMC REG_CONFIG ERROR ");
            return ESP_FAIL;
        }
        
        // Fan Config: bit5=1 (direct setting), bit4=0 (non-inverted), bit3=0 (PWM), bits2-0=011 (PWM freq)
        ret = i2c_bitforge_register_write_byte(EMC2101_dev_handle, EMC2101_FAN_CONFIG, 0b00100011);
        if (ret != ESP_OK) {
            ESP_LOGI(TAG, "EMC FAN_CONFIG ERROR ");
            return ESP_FAIL;
        }
        
        // Fan Spin Up Configuration: Set drive level and time for reliable startup
        // bits 7-5: spin drive level, bits 4-3: spin up time, bits 2-0: disable spin routine
        ret = i2c_bitforge_register_write_byte(EMC2101_dev_handle, EMC2101_FAN_SPINUP, 0b01101000);
        if (ret != ESP_OK) {
            ESP_LOGI(TAG, "EMC FAN_SPINUP ERROR ");
            return ESP_FAIL;
        }
        
        // Set PWM frequency divider for smoother control
        ret = i2c_bitforge_register_write_byte(EMC2101_dev_handle, EMC2101_PWM_FREQ, 0x1F);
        if (ret != ESP_OK) {
            ESP_LOGI(TAG, "EMC PWM_FREQ ERROR ");
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

void EMC2101_setIdealityFactor(uint8_t idealityFactor) {
    //set Ideality Factor
    esp_err_t err = i2c_bitforge_register_write_byte(EMC2101_dev_handle, EMC2101_IDEALITY_FACTOR, idealityFactor);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set ideality factor: %s", esp_err_to_name(err));
    }
}


void EMC2101_setBetaCompensation(uint8_t betaFactor) {
    //set Beta Compensation
    esp_err_t err = i2c_bitforge_register_write_byte(EMC2101_dev_handle, EMC2101_BETA_COMPENSATION, betaFactor);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set beta compensation: %s", esp_err_to_name(err));
    }
}

// takes a fan speed percent (0.0-1.0)
void EMC2101_setFanSpeed(float percent)
{
    uint8_t speed;
    speed = (uint8_t) (63.0 * percent);
    esp_err_t err = i2c_bitforge_register_write_byte(EMC2101_dev_handle, EMC2101_REG_FAN_SETTING, speed);
    if (err != ESP_OK) {
        // A transient NACK here must not be fatal: the fan keeps its previous
        // setting and the next control-loop iteration retries.
        ESP_LOGE(TAG, "Failed to set fan speed: %s", esp_err_to_name(err));
    }
}

// RPM = 5400000/reading
uint16_t EMC2101_getFanSpeed(void)
{
    uint8_t tach_lsb = 0, tach_msb = 0;
    uint16_t reading;
    uint16_t RPM;
    esp_err_t err;

    err = i2c_bitforge_register_read(EMC2101_dev_handle, EMC2101_TACH_LSB, &tach_lsb, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read tach LSB: %s", esp_err_to_name(err));
        return 0;
    }

    err = i2c_bitforge_register_read(EMC2101_dev_handle, EMC2101_TACH_MSB, &tach_msb, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read tach MSB: %s", esp_err_to_name(err));
        return 0;
    }

    // ESP_LOGI(TAG, "Raw Fan Speed = %02X %02X", tach_msb, tach_lsb);

    reading = tach_lsb | (tach_msb << 8);
    if (reading == 0) {
        // A zero tach reading would trap on the divide below.
        ESP_LOGW(TAG, "Invalid tach reading of 0");
        return 0;
    }
    RPM = 5400000 / reading;

    // ESP_LOGI(TAG, "Fan Speed = %d RPM", RPM);
    if (RPM == 82) {
        return 0;
    }
    return RPM;
}

float EMC2101_getExternalTemp(void)
{
    uint8_t temp_msb = 0, temp_lsb = 0;
    uint16_t reading;
    esp_err_t err;

    err = i2c_bitforge_register_read(EMC2101_dev_handle, EMC2101_EXTERNAL_TEMP_MSB, &temp_msb, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read external temperature MSB: %s", esp_err_to_name(err));
        return 0.0f;
    }
    
    err = i2c_bitforge_register_read(EMC2101_dev_handle, EMC2101_EXTERNAL_TEMP_LSB, &temp_lsb, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read external temperature LSB: %s", esp_err_to_name(err));
        return 0.0f;
    }
    
    // Combine MSB and LSB, and then right shift to get 11 bits
    reading = (temp_msb << 8) | temp_lsb;
    reading >>= 5;  // Now, `reading` contains an 11-bit signed value

    // Cast `reading` to a signed 16-bit integer
    int16_t signed_reading = (int16_t)reading;

    // If the 11th bit (sign bit in 11-bit data) is set, extend the sign
    if (signed_reading & 0x0400) {
        signed_reading |= 0xF800;  // Set upper bits to extend the sign
    }

    if (signed_reading == EMC2101_TEMP_FAULT_OPEN_CIRCUIT) {
        ESP_LOGE(TAG, "EMC2101 TEMP_FAULT_OPEN_CIRCUIT: %04X", signed_reading);
        return 0.0f;
    }
    if (signed_reading == EMC2101_TEMP_FAULT_SHORT) {
        ESP_LOGE(TAG, "EMC2101 TEMP_FAULT_SHORT: %04X", signed_reading);
        return 0.0f;
    }

    // Convert the signed reading to temperature in Celsius
    float result = (float)signed_reading / 8.0;

    return result;
}

float EMC2101_getInternalTemp(void)
{
    uint8_t temp = 0;
    esp_err_t err = i2c_bitforge_register_read(EMC2101_dev_handle, EMC2101_INTERNAL_TEMP, &temp, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read internal temperature: %s", esp_err_to_name(err));
        return 0.0f;
    }
    float result = (float)temp;
    return result;
}

