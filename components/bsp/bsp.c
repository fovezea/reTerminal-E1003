/*
 * bsp.c — Board Support Package implementation for reTerminal E1003
 */

#include "bsp.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "hal/adc_types.h"
#include "esp_adc/adc_oneshot.h"

static const char *TAG = "bsp";

/* I2C0 bus handle — created once, used by the scanner and peripherals */
static i2c_master_bus_handle_t s_i2c0_handle = NULL;

/* ==========================================================================
 * I2C0 helpers (new driver API — ESP-IDF v6)
 * ========================================================================== */

static esp_err_t bsp_i2c0_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port    = I2C_NUM_0,
        .sda_io_num  = BSP_I2C0_SDA,
        .scl_io_num  = BSP_I2C0_SCL,
        .clk_source  = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    esp_err_t ret = i2c_new_master_bus(&cfg, &s_i2c0_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: 0x%x", ret);
        return ret;
    }

    ESP_LOGI(TAG, "I2C0 ready: SDA=GPIO%d SCL=GPIO%d",
             BSP_I2C0_SDA, BSP_I2C0_SCL);
    return ESP_OK;
}

/* ==========================================================================
 * I2C bus scanner (new driver — i2c_master_probe)
 * ========================================================================== */

static void bsp_i2c_scan(void)
{
    if (!s_i2c0_handle) return;

    ESP_LOGI(TAG, "I2C0 scan:");
    for (uint8_t addr = 1; addr < 127; addr++) {
        if (i2c_master_probe(s_i2c0_handle, addr, 10) == ESP_OK) {
            ESP_LOGI(TAG, "  0x%02X found", addr);
        }
    }
}

/* ==========================================================================
 * Public API
 * ========================================================================== */

esp_err_t bsp_display_init(void)
{
    /* Panel power enable */
    gpio_config_t pwr = {
        .pin_bit_mask = BIT64(BSP_DISP_VCC_EN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr);
    gpio_set_level(BSP_DISP_VCC_EN, 1);

    /* Reset pin */
    gpio_config_t rst = {
        .pin_bit_mask = BIT64(BSP_DISP_RST),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst);
    gpio_set_level(BSP_DISP_RST, 1);

    /* DC / EN */
    gpio_config_t dc = {
        .pin_bit_mask = BIT64(BSP_DISP_DC),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&dc);
    gpio_set_level(BSP_DISP_DC, 0);

    /* BUSY — input */
    gpio_config_t busy = {
        .pin_bit_mask = BIT64(BSP_DISP_BUSY),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&busy);

    /* CS — output, idle HIGH */
    gpio_config_t cs = {
        .pin_bit_mask = BIT64(BSP_DISP_CS),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cs);
    gpio_set_level(BSP_DISP_CS, 1);

    ESP_LOGI(TAG, "Display GPIOs: %dx%d, IT8951", BSP_LCD_WIDTH, BSP_LCD_HEIGHT);
    return ESP_OK;
}

esp_err_t bsp_touch_init(void)
{
    /* INT pin — input with pull-up (active LOW from GT911) */
    gpio_config_t int_pin = {
        .pin_bit_mask = BIT64(BSP_TOUCH_INT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_pin);

    /* RESET pin — output, pulse LOW then HIGH to reset the controller */
    gpio_config_t rst_pin = {
        .pin_bit_mask = BIT64(BSP_TOUCH_RST),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_pin);

    /* Reset sequence per GT911 datasheet */
    gpio_set_level(BSP_TOUCH_RST, 0);
    esp_rom_delay_us(100);
    gpio_set_level(BSP_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(TAG, "Touch (GT911) GPIOs: INT=%d RST=%d", BSP_TOUCH_INT, BSP_TOUCH_RST);
    return ESP_OK;
}

esp_err_t bsp_sdcard_init(void)
{
    /* Power enable */
    gpio_config_t pwr = {
        .pin_bit_mask = BIT64(BSP_SD_PWR_EN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr);
    gpio_set_level(BSP_SD_PWR_EN, 1);

    /* Chip select — output, idle HIGH */
    gpio_config_t cs = {
        .pin_bit_mask = BIT64(BSP_SD_CS),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cs);
    gpio_set_level(BSP_SD_CS, 1);

    /* Card detect */
    gpio_config_t det = {
        .pin_bit_mask = BIT64(BSP_SD_DET),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&det);

    ESP_LOGI(TAG, "SD card GPIOs: CS=%d DET=%d PWR=%d",
             BSP_SD_CS, BSP_SD_DET, BSP_SD_PWR_EN);
    return ESP_OK;
}

esp_err_t bsp_mic_init(void)
{
    /* Power enable */
    gpio_config_t pwr = {
        .pin_bit_mask = BIT64(BSP_MIC_PWR_EN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr);
    gpio_set_level(BSP_MIC_PWR_EN, 1);

    /* CLK and DATA — held in default state until I2S-PDM driver claims them */
    gpio_config_t clk = {
        .pin_bit_mask = BIT64(BSP_MIC_CLK),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&clk);
    gpio_set_level(BSP_MIC_CLK, 0);

    gpio_config_t data = {
        .pin_bit_mask = BIT64(BSP_MIC_DATA),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&data);

    ESP_LOGI(TAG, "PDM mic powered: CLK=%d DATA=%d PWR=%d",
             BSP_MIC_CLK, BSP_MIC_DATA, BSP_MIC_PWR_EN);
    return ESP_OK;
}

esp_err_t bsp_buzzer_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(BSP_BUZZER_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(BSP_BUZZER_PIN, 0);
    ESP_LOGI(TAG, "Buzzer pin %d configured", BSP_BUZZER_PIN);
    return ESP_OK;
}

esp_err_t bsp_battery_init(void)
{
    /* Load switch — OFF by default */
    gpio_config_t sw = {
        .pin_bit_mask = BIT64(BSP_BAT_SWITCH),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&sw);
    gpio_set_level(BSP_BAT_SWITCH, 0);

    ESP_LOGI(TAG, "Battery monitor: ADC_CH%d, switch GPIO%d",
             BSP_BAT_ADC_CHANNEL, BSP_BAT_SWITCH);
    return ESP_OK;
}

esp_err_t bsp_battery_read_mv(uint32_t *voltage_mv)
{
    if (voltage_mv == NULL) return ESP_ERR_INVALID_ARG;

    adc_oneshot_unit_handle_t adc_handle = NULL;
    adc_oneshot_unit_init_cfg_t adc_cfg = {
        .unit_id = BSP_BAT_ADC_UNIT,
    };
    esp_err_t ret = adc_oneshot_new_unit(&adc_cfg, &adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %d", ret);
        return ret;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_oneshot_config_channel(adc_handle, BSP_BAT_ADC_CHANNEL, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %d", ret);
        adc_oneshot_del_unit(adc_handle);
        return ret;
    }

    /* Enable the load switch */
    gpio_set_level(BSP_BAT_SWITCH, 1);
    vTaskDelay(pdMS_TO_TICKS(5));

    int raw = 0;
    ret = adc_oneshot_read(adc_handle, BSP_BAT_ADC_CHANNEL, &raw);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_read failed: %d", ret);
        gpio_set_level(BSP_BAT_SWITCH, 0);
        adc_oneshot_del_unit(adc_handle);
        return ret;
    }

    /* Disable the load switch */
    gpio_set_level(BSP_BAT_SWITCH, 0);
    adc_oneshot_del_unit(adc_handle);

    /* 12-bit ADC, 12 dB atten → ~2500 mV full-scale.
     * Multiply by 2 for the external voltage divider. */
    const int mv_at_adc = (raw * 2500) / 4096;
    *voltage_mv = (uint32_t)((float)mv_at_adc * BSP_BAT_DIVIDER);

    return ESP_OK;
}

/* ==========================================================================
 * Full init
 * ========================================================================== */

esp_err_t bsp_init(void)
{
    esp_err_t ret = ESP_OK;
    esp_err_t step;

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  reTerminal E1003 BSP initialising ...");
    ESP_LOGI(TAG, "========================================");

    /* --- I2C0 --- */
    step = bsp_i2c0_init();
    if (step != ESP_OK) { ESP_LOGE(TAG, "I2C0 init failed"); ret = step; }

    /* --- LED --- */
    gpio_config_t led = {
        .pin_bit_mask = BIT64(BSP_LED_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led);
    BSP_LED_OFF();

    /* --- Buttons --- */
    gpio_config_t btn = {
        .pin_bit_mask = BIT64(BSP_BTN_KEY0) | BIT64(BSP_BTN_KEY1) | BIT64(BSP_BTN_KEY2),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);

    /* --- Display --- */
    step = bsp_display_init();
    if (step != ESP_OK) { ESP_LOGE(TAG, "Display init failed"); ret = step; }

    /* --- Touch --- */
    step = bsp_touch_init();
    if (step != ESP_OK) { ESP_LOGE(TAG, "Touch init failed"); ret = step; }

    /* --- SD card --- */
    step = bsp_sdcard_init();
    if (step != ESP_OK) { ESP_LOGE(TAG, "SD card init failed"); ret = step; }

    /* --- Buzzer --- */
    step = bsp_buzzer_init();
    if (step != ESP_OK) { ESP_LOGE(TAG, "Buzzer init failed"); ret = step; }

    /* --- Battery --- */
    step = bsp_battery_init();
    if (step != ESP_OK) { ESP_LOGE(TAG, "Battery init failed"); ret = step; }

    /* --- I2C bus scan --- */
    bsp_i2c_scan();

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "BSP init complete — all peripherals ready.");
    } else {
        ESP_LOGW(TAG, "BSP init finished with errors (check logs above).");
    }

    return ret;
}
