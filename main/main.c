#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "bsp.h"
#include "lvgl.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "reTerminal E1003 — LVGL demo  [wake: %s]",
             bsp_wake_cause_str());
    ESP_LOGI(TAG, "PSRAM: %u MB", (unsigned)(esp_psram_get_size() >> 20));

    bsp_init();

    uint32_t bat_mv;
    bsp_battery_read_mv(&bat_mv);
    ESP_LOGI(TAG, "Battery: %lu mV", (unsigned long)bat_mv);

    /* ---- LVGL display, touch, and tick ---- */
    ESP_ERROR_CHECK(bsp_lvgl_display_init());
    ESP_ERROR_CHECK(bsp_lvgl_touch_init());
    ESP_ERROR_CHECK(bsp_lvgl_tick_init());

    /* ---- Create a simple screen ---- */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Title label */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "reTerminal E1003");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 80);

    /* Subtitle */
    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "LVGL on IT8951 e-Paper");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(sub, lv_color_black(), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 150);

    /* Info line */
    lv_obj_t *info = lv_label_create(scr);
    lv_label_set_text_fmt(info,
        "Panel: 1872 x 1404  |  ED103TC2  |  %lu mV",
        (unsigned long)bat_mv);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(info, lv_color_black(), 0);
    lv_obj_align(info, LV_ALIGN_CENTER, 0, 0);

    /* Footer */
    lv_obj_t *footer = lv_label_create(scr);
    lv_label_set_text(footer, "ESP32-S3  |  IT8951  |  LVGL v9");
    lv_obj_set_style_text_font(footer, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(footer, lv_color_black(), 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -40);

    /* Let LVGL render the screen — flush happens asynchronously.
     * lv_timer_handler() drives the display refresh timer. */
    ESP_LOGI(TAG, "Rendering screen...");
    for (int i = 0; i < 20; i++) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "Screen rendered, sleeping in 30 s...");

    vTaskDelay(pdMS_TO_TICKS(30000));

    bsp_deep_sleep_enter(30, true);
}
