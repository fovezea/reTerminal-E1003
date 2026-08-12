/*
 * lvgl_port.cpp — LVGL integration for reTerminal E1003
 *
 * Architecture (proven working):
 *   LVGL renders RGB565 into a PARTIAL buffer.
 *   Flush callback converts RGB565 → 8BPP (0x00 black, 0xFF white)
 *   into a full-screen 8BPP framebuffer.
 *   After LVGL, panel_update() sends via it8951_write_8bpp_frame()
 *   which uses the proven 8BPP expanded path with bulk xfer8n.
 */

#include "bsp.h"
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "it8951.h"
#include "lvgl.h"

static const char *TAG = "lvgl_port";

static lv_display_t  *s_disp  = NULL;
static lv_indev_t    *s_indev = NULL;
static i2c_master_dev_handle_t s_touch_dev = NULL;

/* ==========================================================================
 * Flush callback — RGB565 → 4BPP packed framebuffer
 * ========================================================================== */

/* Convert RGB565 → 4-bit gray (0=black, 15=white), perceptual weights. */
static uint8_t rgb565_to_4bit(uint16_t c)
{
    uint32_t r = ((c >> 11) & 0x1F) * 255 / 31;
    uint32_t g = ((c >>  5) & 0x3F) * 255 / 63;
    uint32_t b = ( c        & 0x1F) * 255 / 31;
    uint32_t lum = (r * 30 + g * 59 + b * 11) / 100; /* 0..255 */
    return (uint8_t)(lum >> 4);  /* 0..15 */
}

/* 4BPP framebuffer: 2 pixels per byte, even x = high nibble */
static uint8_t *s_fb_4bpp = NULL;

static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area,
                          uint8_t *px_map)
{
    uint16_t w = (uint16_t)(area->x2 - area->x1 + 1);
    uint16_t h = (uint16_t)(area->y2 - area->y1 + 1);
    const uint16_t *pixels = (const uint16_t *)px_map;

    /* Convert RGB565 → 4bpp and pack directly into 4BPP framebuffer.
     * Anti-aliased edges preserved as gray levels 0–15. */
    for (uint16_t row = 0; row < h; row++) {
        uint32_t y = (uint32_t)(area->y1 + row);
        for (uint16_t col = 0; col < w; col++) {
            uint32_t x = (uint32_t)(area->x1 + col);
            uint8_t gray = rgb565_to_4bit(pixels[row * w + col]);
            /* Pack: even x → high nibble, odd x → low nibble */
            uint32_t bi = ((uint32_t)y * BSP_LCD_WIDTH + x) / 2;
            if (x & 1)
                s_fb_4bpp[bi] = (s_fb_4bpp[bi] & 0xF0) | gray;
            else
                s_fb_4bpp[bi] = (s_fb_4bpp[bi] & 0x0F) | (gray << 4);
        }
    }

    lv_display_flush_ready(disp);
}

/* ==========================================================================
 * Panel update — proven 8BPP path
 * ========================================================================== */
static void panel_update(void)
{
    /* Send pre-packed 4BPP framebuffer directly — no conversion needed */
    it8951_write_4bpp_packed(s_fb_4bpp);
}

/* ==========================================================================
 * Display init
 * ========================================================================== */
esp_err_t bsp_lvgl_display_init(void)
{
    /* 4BPP packed framebuffer: 1872 × 1404 / 2 = 1.25 MB.
     * Flush callback packs RGB565 → 4-bit nibbles directly. */
    size_t fb_size = (size_t)BSP_LCD_WIDTH * BSP_LCD_HEIGHT / 2;
    s_fb_4bpp = (uint8_t *)heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM);
    if (!s_fb_4bpp) { ESP_LOGE(TAG, "4BPP fb alloc failed"); return ESP_ERR_NO_MEM; }
    memset(s_fb_4bpp, 0xFF, fb_size);  /* all white (0xFF = 0x0F·0x0F) */

    esp_err_t ret = it8951_init();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "IT8951 init failed"); return ret; }

    lv_init();

    s_disp = lv_display_create(BSP_LCD_WIDTH, BSP_LCD_HEIGHT);
    lv_display_set_flush_cb(s_disp, disp_flush_cb);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);

    const uint32_t buf_lines = 20;
    size_t buf_bytes = (size_t)BSP_LCD_WIDTH * buf_lines * 2;  /* RGB565 */
    uint8_t *lv_buf = (uint8_t *)heap_caps_malloc(buf_bytes,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lv_buf) { ESP_LOGE(TAG, "LVGL buf alloc failed"); return ESP_ERR_NO_MEM; }

    lv_display_set_buffers(s_disp, lv_buf, NULL, buf_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_LOGI(TAG, "4BPP packed fb=%u KB, LVGL buf=%u lines",
             (unsigned)(fb_size / 1024), (unsigned)buf_lines);
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
    if (i2c_master_transmit_receive(s_touch_dev, tx, 2, &status, 1, 5) != ESP_OK)
    { data->state = LV_INDEV_STATE_RELEASED; return; }
    if ((status & 0x0F) == 0) { data->state = LV_INDEV_STATE_RELEASED; return; }
    tx[1] = 0x4F;
    uint8_t raw[5] = {0};
    if (i2c_master_transmit_receive(s_touch_dev, tx, 2, raw, 5, 5) != ESP_OK)
    { data->state = LV_INDEV_STATE_RELEASED; return; }
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
static void lvgl_tick_cb(void *arg) { lv_tick_inc(5); }

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

/* ==========================================================================
 * Public API
 * ========================================================================== */
void bsp_lvgl_panel_update(void)
{
    panel_update();
}

void bsp_lvgl_touch_deinit(void)
{
    if (s_touch_dev) {
        i2c_master_bus_rm_device(s_touch_dev);
        s_touch_dev = NULL;
    }
}
