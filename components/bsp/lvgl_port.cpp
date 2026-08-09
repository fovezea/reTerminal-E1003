/*
 * lvgl_port.cpp — LVGL integration for reTerminal E1003
 *
 * Display: IT8951 (1bpp, via local it8951.c driver)
 * Touch:   GT911 (I2C0)
 */

#include "bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "it8951.h"
#include "lvgl.h"

static const char *TAG = "lvgl_port";

static lv_display_t  *s_disp  = NULL;
static lv_indev_t    *s_indev = NULL;
static i2c_master_dev_handle_t s_touch_dev = NULL;

/* 1bpp framebuffer: 1872 * 1404 / 8 = 328,536 bytes */
static uint8_t *s_fb = NULL;

/* ==========================================================================
 * Display flush — 1bpp rendering
 * ========================================================================== */

static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area,
                          uint8_t *px_map)
{
    uint16_t w = (uint16_t)(area->x2 - area->x1 + 1);
    uint16_t h = (uint16_t)(area->y2 - area->y1 + 1);

    /* px_map is 1bpp: each byte = 8 horizontal pixels, MSB leftmost.
     * Row stride = (w + 7) / 8 bytes. */
    uint16_t row_bytes = (w + 7) / 8;

    ESP_LOGI(TAG, "Flush start: %dx%d @ (%d,%d)", w, h, area->x1, area->y1);

    it8951_load_start((uint16_t)area->x1, (uint16_t)area->y1, w, h);
    ESP_LOGI(TAG, "  load_start done");

    for (uint16_t row = 0; row < h; row++) {
        it8951_load_flush(px_map + (size_t)row * row_bytes, row_bytes);
    }
    ESP_LOGI(TAG, "  load_flush done (%d rows)", h);

    it8951_load_end();
    ESP_LOGI(TAG, "  load_end done");

    it8951_display_area((uint16_t)area->x1, (uint16_t)area->y1, w, h);
    ESP_LOGI(TAG, "  display_area done");

    lv_display_flush_ready(disp);
}

/* ==========================================================================
 * Display init
 * ========================================================================== */

esp_err_t bsp_lvgl_display_init(void)
{
    esp_err_t ret = it8951_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "IT8951 init failed");
        return ret;
    }

    lv_init();

    s_disp = lv_display_create(BSP_LCD_WIDTH, BSP_LCD_HEIGHT);
    lv_display_set_flush_cb(s_disp, disp_flush_cb);

    /* Small partial buffer — each flush completes faster */
    uint32_t fb_rows = 70;  /* ~5% of screen height = quick flushes */
    size_t fb_size = (size_t)BSP_LCD_WIDTH * fb_rows / 8;
    s_fb = (uint8_t *)heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM);
    assert(s_fb);
    memset(s_fb, 0xFF, fb_size);

    lv_display_set_buffers(s_disp, s_fb, NULL, fb_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_LOGI(TAG, "LVGL: %dx%d 1bpp, partial fb=%u B", BSP_LCD_WIDTH, BSP_LCD_HEIGHT,
             (unsigned)fb_size);
    return ESP_OK;
}

/* ==========================================================================
 * Touch (GT911)
 * ========================================================================== */

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    if (!s_touch_dev) { data->state = LV_INDEV_STATE_RELEASED; return; }

    uint8_t tx[2] = {0x81, 0x4E};
    uint8_t status = 0;
    if (i2c_master_transmit_receive(s_touch_dev, tx, 2, &status, 1, 5) != ESP_OK) {
        data->state = LV_INDEV_STATE_RELEASED; return;
    }
    if ((status & 0x0F) == 0) {
        data->state = LV_INDEV_STATE_RELEASED; return;
    }

    tx[1] = 0x4F;
    uint8_t raw[5] = {0};
    if (i2c_master_transmit_receive(s_touch_dev, tx, 2, raw, 5, 5) != ESP_OK) {
        data->state = LV_INDEV_STATE_RELEASED; return;
    }
    i2c_master_transmit(s_touch_dev, tx, 2, 5);

    data->point.x = ((uint16_t)raw[1] << 8) | raw[0];
    data->point.y = ((uint16_t)raw[3] << 8) | raw[2];
    data->state   = LV_INDEV_STATE_PRESSED;
}

esp_err_t bsp_lvgl_touch_init(void)
{
    vTaskDelay(pdMS_TO_TICKS(100));

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = BSP_I2C_ADDR_GT911_2;
    dev_cfg.scl_speed_hz    = BSP_I2C_CLK_FAST;

    i2c_master_bus_handle_t i2c_handle = bsp_i2c0_get_handle();
    esp_err_t ret = i2c_master_bus_add_device(i2c_handle, &dev_cfg, &s_touch_dev);
    if (ret != ESP_OK) {
        dev_cfg.device_address = BSP_I2C_ADDR_GT911_1;
        ret = i2c_master_bus_add_device(i2c_handle, &dev_cfg, &s_touch_dev);
    }
    if (ret != ESP_OK) { ESP_LOGE(TAG, "GT911 not found"); return ret; }

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_read_cb);
    ESP_LOGI(TAG, "GT911 ready");
    return ESP_OK;
}

/* ==========================================================================
 * Tick
 * ========================================================================== */

static int s_tick_count = 0;
static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(5);
    if (++s_tick_count == 200) {  /* every 1 second */
        ESP_LOGI(TAG, "Tick alive");
        s_tick_count = 0;
    }
}

esp_err_t bsp_lvgl_tick_init(void)
{
    esp_timer_handle_t timer;
    esp_timer_create_args_t args = {};
    args.callback = lvgl_tick_cb;
    args.name     = "lvgl_tick";
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 5000));
    return ESP_OK;
}
