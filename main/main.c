#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "bsp.h"
#include "it8951.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "reTerminal E1003 — direct IT8951 test  [wake: %s]",
             bsp_wake_cause_str());
    ESP_LOGI(TAG, "PSRAM: %u MB", (unsigned)(esp_psram_get_size() >> 20));

    bsp_init();
    uint32_t bat_mv;
    bsp_battery_read_mv(&bat_mv);
    ESP_LOGI(TAG, "Battery: %lu mV", (unsigned long)bat_mv);

    /* Init display */
    esp_err_t err = it8951_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IT8951 init failed");
        bsp_deep_sleep_enter(30, true);
    }

    ESP_LOGI(TAG, "Clear screen (INIT mode — will take ~30s)...");
    it8951_clear_screen();
    ESP_LOGI(TAG, "Screen cleared!");

    /* Draw a simple pattern: black rectangle in the center */
    uint16_t w = 400, h = 200;
    uint16_t x = (BSP_LCD_WIDTH - w) / 2;
    uint16_t y = (BSP_LCD_HEIGHT - h) / 2;
    uint16_t row_bytes = (w + 7) / 8;

    ESP_LOGI(TAG, "Drawing %dx%d rect at (%d,%d)...", w, h, x, y);

    it8951_load_start(x, y, w, h);

    /* Send all-black rows (1bpp: 0x00 bytes = black) */
    uint8_t black_row[row_bytes];
    memset(black_row, 0x00, row_bytes);
    for (uint16_t row = 0; row < h; row++) {
        it8951_load_flush(black_row, row_bytes);
    }

    it8951_load_end();
    it8951_display_area(x, y, w, h);

    ESP_LOGI(TAG, "Rectangle drawn! Sleeping in 10s...");
    vTaskDelay(pdMS_TO_TICKS(10000));

    bsp_deep_sleep_enter(30, true);
}
