#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#define GPIO_BUTTON_BOOT CONFIG_GPIO_BUTTON_BOOT

#define LONG_PRESS_DURATION_MS 2000
#define DEBOUNCE_MS 30
#define POLL_INTERVAL_MS 20

static const char * TAG = "input";

static TaskHandle_t button_task_handle = NULL;

static void (*button_short_clicked_fn)(void) = NULL;
static void (*button_long_pressed_fn)(void) = NULL;

static bool button_is_pressed(void)
{
    return gpio_get_level(GPIO_BUTTON_BOOT) == 0; // LOW when pressed
}

static void IRAM_ATTR button_isr_handler(void *arg)
{
    BaseType_t higher_prio_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(button_task_handle, &higher_prio_task_woken);
    portYIELD_FROM_ISR(higher_prio_task_woken);
}

static void button_task(void *pvParameters)
{
    while (1) {
        // Sleep until an edge wakes us up
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
        if (!button_is_pressed()) {
            continue; // bounce or a release we already handled
        }

        // Held down: fire the long press as soon as the threshold passes, like the
        // ISR-free equivalent of LV_EVENT_LONG_PRESSED did
        bool long_press_fired = false;
        int64_t press_start = esp_timer_get_time();

        while (button_is_pressed()) {
            if (!long_press_fired && (esp_timer_get_time() - press_start) >= LONG_PRESS_DURATION_MS * 1000LL) {
                long_press_fired = true;
                if (button_long_pressed_fn != NULL) {
                    ESP_LOGI(TAG, "Long button press detected");
                    button_long_pressed_fn();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        }

        if (!long_press_fired && button_short_clicked_fn != NULL) {
            ESP_LOGI(TAG, "Short button click detected");
            button_short_clicked_fn();
        }

        // Drop the edges the release bounce queued up while we were polling
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
        ulTaskNotifyTake(pdTRUE, 0);
    }
}

esp_err_t input_init(void (*button_short_clicked_cb)(void), void (*button_long_pressed_cb)(void))
{
    ESP_LOGI(TAG, "Install button driver");

    button_short_clicked_fn = button_short_clicked_cb;
    button_long_pressed_fn = button_long_pressed_cb;

    // Button handling
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_BUTTON_BOOT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "Button GPIO config failed");

    // The ISR service is installed during peripheral init, but self test and normal
    // boot get there by different paths, so tolerate it already being up
    esp_err_t isr_service_err = gpio_install_isr_service(0);
    if (isr_service_err != ESP_OK && isr_service_err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(isr_service_err, TAG, "Error installing ISR service");
    }

    // Task has to exist before the ISR can notify it
    ESP_RETURN_ON_FALSE(xTaskCreate(button_task, "button", 3072, NULL, 5, &button_task_handle) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "Failed to create button task");

    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(GPIO_BUTTON_BOOT, button_isr_handler, NULL), TAG, "Error adding ISR handler");

    return ESP_OK;
}
