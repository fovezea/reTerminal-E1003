#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"

static const char *TAG = "reTerminal-E1003";

void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "reTerminal E1003 – ESP-IDF v6");
    ESP_LOGI(TAG, "Chip: %s, cores: %d, rev: %d",
             CONFIG_IDF_TARGET,
             chip_info.cores,
             chip_info.revision);

    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "Flash: %lu MB (%s)",
                 (unsigned long)(flash_size >> 20),
                 (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
    }

    ESP_LOGI(TAG, "PSRAM: %s",
             (chip_info.features & CHIP_FEATURE_EMB_PSRAM) ? "present" : "not detected");

    for (int i = 10; i > 0; i--) {
        ESP_LOGI(TAG, "Starting in %d...", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Hello, reTerminal E1003!");
}
