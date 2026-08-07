#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "bsp.h"

static const char *TAG = "app";

void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "reTerminal E1003 — ESP-IDF v6");
    ESP_LOGI(TAG, "Target: %s | cores: %d | rev: %d",
             CONFIG_IDF_TARGET,
             chip_info.cores,
             chip_info.revision);

    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "Flash: %lu MB (%s)",
                 (unsigned long)(flash_size >> 20),
                 (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
    }

    size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "PSRAM: %s (%u MB)",
             psram_size > 0 ? "present" : "not detected",
             (unsigned)(psram_size >> 20));

    /* Initialise the board */
    esp_err_t err = bsp_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BSP init returned error: 0x%x", err);
    }

    /* Read battery */
    uint32_t bat_mv = 0;
    if (bsp_battery_read_mv(&bat_mv) == ESP_OK) {
        ESP_LOGI(TAG, "Battery: %lu mV (%.2f V)",
                 (unsigned long)bat_mv, bat_mv / 1000.0f);
    }

    /* Blink the LED a few times */
    for (int i = 0; i < 3; i++) {
        BSP_LED_ON();
        vTaskDelay(pdMS_TO_TICKS(200));
        BSP_LED_OFF();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ESP_LOGI(TAG, "Board ready — entering idle loop.");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
