#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "bsp.h"
#include "it8951.h"
#include "lvgl.h"

static const char *TAG = "app";

void app_main(void)
{
    const char *wake = bsp_wake_cause_str();
    bool cold_boot = (strcmp(wake, "power-on / reset") == 0);
    ESP_LOGI(TAG, "reTerminal E1003 — LVGL 8BPP  [wake: %s]", wake);
    ESP_LOGI(TAG, "PSRAM: %u MB", (unsigned)(esp_psram_get_size() >> 20));

    bsp_init();

    uint32_t bat_mv;
    bsp_battery_read_mv(&bat_mv);
    ESP_LOGI(TAG, "Battery: %lu mV", (unsigned long)bat_mv);

    float temp_c = 0, humidity = 0;
    bsp_sht4x_read(&temp_c, &humidity);
    ESP_LOGI(TAG, "SHT4x: %.1f C, %.0f %%", temp_c, humidity);

    ESP_ERROR_CHECK(bsp_lvgl_display_init());
    ESP_ERROR_CHECK(bsp_lvgl_touch_init());
    ESP_ERROR_CHECK(bsp_lvgl_tick_init());

    if (cold_boot) it8951_clean_screen();

    /* Simple LVGL screen */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "reTerminal E1003");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    char buf[64];
    lv_obj_t *bat = lv_label_create(scr);
    snprintf(buf, sizeof(buf), "Battery: %lu mV", (unsigned long)bat_mv);
    lv_label_set_text(bat, buf);
    lv_obj_set_style_text_font(bat, &lv_font_montserrat_28, 0);
    lv_obj_align(bat, LV_ALIGN_CENTER, 0, -50);

    lv_obj_t *temp = lv_label_create(scr);
    snprintf(buf, sizeof(buf), "%.1f C", temp_c);
    lv_label_set_text(temp, buf);
    lv_obj_set_style_text_font(temp, &lv_font_montserrat_32, 0);
    lv_obj_align(temp, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *hum = lv_label_create(scr);
    snprintf(buf, sizeof(buf), "%.0f %% RH", humidity);
    lv_label_set_text(hum, buf);
    lv_obj_set_style_text_font(hum, &lv_font_montserrat_28, 0);
    lv_obj_align(hum, LV_ALIGN_CENTER, 0, 50);

    ESP_LOGI(TAG, "Rendering...");
    lv_tick_inc(5);
    lv_timer_handler();
    bsp_lvgl_panel_update();
    ESP_LOGI(TAG, "Done.");

    vTaskDelay(pdMS_TO_TICKS(30000));
    bsp_deep_sleep_enter(30, true);
}
