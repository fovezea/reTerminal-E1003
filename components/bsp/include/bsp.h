/*
 * bsp.h — Board Support Package for Seeed Studio reTerminal E1003
 *
 * MCU        : ESP32-S3-WROOM-1-N16R8  (16 MB Flash, 8 MB Octal PSRAM)
 * Display    : ED103TC2 10.3" e-Paper, 1872×1404, 16-level gray, IT8951 controller
 * Touch      : GT911 (I2C0, addr 0x5D / 0x14)
 * RTC        : PCF8563M/TR (I2C0, addr 0x51)
 * Sensor     : SHT4x (I2C0, addr 0x44)
 * Mic        : PDM (GPIO41/42)
 * Storage    : MicroSD on SPI2 (shared bus with display, separate CS)
 *
 * All pin assignments are extracted from the manufacturer's Arduino examples and
 * the TRMNL reference firmware under OSHW-reTerminal/examples/.
 */

#pragma once

#include "driver/gpio.h"
#include "hal/gpio_types.h"
#include "hal/adc_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * Display — ED103TC2 / IT8951, SPI2 (FSPI)
 * ========================================================================== */

#define BSP_DISP_SCK          GPIO_NUM_7
#define BSP_DISP_MOSI         GPIO_NUM_9
#define BSP_DISP_MISO         GPIO_NUM_8
#define BSP_DISP_CS           GPIO_NUM_10
#define BSP_DISP_RST          GPIO_NUM_12
#define BSP_DISP_DC           GPIO_NUM_11    /* data / command */
#define BSP_DISP_BUSY         GPIO_NUM_13    /* HRDY */
#define BSP_DISP_VCC_EN       GPIO_NUM_21    /* panel power enable */

#define BSP_LCD_WIDTH         1872
#define BSP_LCD_HEIGHT        1404
#define BSP_LCD_GRAY_LEVELS   16

/* ==========================================================================
 * Touch — GT911, I2C0
 * ========================================================================== */

#define BSP_TOUCH_SDA          GPIO_NUM_19
#define BSP_TOUCH_SCL          GPIO_NUM_20
#define BSP_TOUCH_INT          GPIO_NUM_2
#define BSP_TOUCH_RST          GPIO_NUM_48

#define BSP_I2C_ADDR_GT911_1   0x5D
#define BSP_I2C_ADDR_GT911_2   0x14

/* ==========================================================================
 * I2C0 bus (shared: touch, RTC, sensor)
 * ========================================================================== */

#define BSP_I2C0_SDA           GPIO_NUM_19
#define BSP_I2C0_SCL           GPIO_NUM_20

/* --- I2C0 device addresses --- */

#define BSP_I2C_ADDR_PCF8563   0x51
#define BSP_I2C_ADDR_SHT4X     0x44

/* ==========================================================================
 * User buttons (active LOW, hardware pull-up)
 * ========================================================================== */

#define BSP_BTN_KEY0           GPIO_NUM_3    /* green button  */
#define BSP_BTN_KEY1           GPIO_NUM_4
#define BSP_BTN_KEY2           GPIO_NUM_5

/* ==========================================================================
 * LED (inverted: LOW = ON, HIGH = OFF)
 * ========================================================================== */

#define BSP_LED_PIN            GPIO_NUM_16
#define BSP_LED_ON()           gpio_set_level(BSP_LED_PIN, 0)
#define BSP_LED_OFF()          gpio_set_level(BSP_LED_PIN, 1)
#define BSP_LED_TOGGLE()       gpio_set_level(BSP_LED_PIN, !gpio_get_level(BSP_LED_PIN))

/* ==========================================================================
 * Buzzer
 * ========================================================================== */

#define BSP_BUZZER_PIN         GPIO_NUM_45

/* ==========================================================================
 * PDM microphone
 * ========================================================================== */

#define BSP_MIC_CLK            GPIO_NUM_42
#define BSP_MIC_DATA           GPIO_NUM_41
#define BSP_MIC_PWR_EN         GPIO_NUM_38

/* ==========================================================================
 * MicroSD card — SPI2 (shared bus with display, separate CS)
 * ========================================================================== */

#define BSP_SD_SCK             GPIO_NUM_7
#define BSP_SD_MOSI            GPIO_NUM_9
#define BSP_SD_MISO            GPIO_NUM_8
#define BSP_SD_CS              GPIO_NUM_14
#define BSP_SD_DET             GPIO_NUM_15     /* LOW = card present */
#define BSP_SD_PWR_EN          GPIO_NUM_39

/* ==========================================================================
 * Battery monitoring (shared ADC pin with KEY0, muxed via load switch)
 * ========================================================================== */

#define BSP_BAT_ADC            GPIO_NUM_3     /* ADC1_CH3 — shared with KEY0 */
#define BSP_BAT_ADC_CHANNEL    ADC_CHANNEL_3
#define BSP_BAT_ADC_UNIT       ADC_UNIT_1
#define BSP_BAT_SWITCH         GPIO_NUM_40    /* load-switch enable, HIGH = battery → ADC */

/* Voltage-divider ratio: the board uses two equal resistors, so we double. */
#define BSP_BAT_DIVIDER        2.0f

/* ==========================================================================
 * Serial debug — USB-UART bridge on carrier (UART1)
 * ========================================================================== */

#define BSP_UART_TX            GPIO_NUM_43
#define BSP_UART_RX            GPIO_NUM_44

/* ==========================================================================
 * I2C bus speed presets
 * ========================================================================== */

#define BSP_I2C_CLK_STANDARD   100000
#define BSP_I2C_CLK_FAST       400000

/* ==========================================================================
 * Initialisation
 * ========================================================================== */

/**
 * @brief Full board initialisation.
 *
 * Powers up peripherals, configures I2C0, enables display and SD-card power,
 * sets button GPIOs to input with pull-ups, and turns the LED off.
 *
 * @return ESP_OK on success, or a combined error code if any step fails.
 */
esp_err_t bsp_init(void);

/**
 * @brief Minimal display GPIO setup (assert RST, enable panel power).
 */
esp_err_t bsp_display_init(void);

/**
 * @brief Probe the IT8951 display controller over SPI.
 *
 * Sends SYS_RUN, reads the device-info registers, and logs panel/FW details.
 * SPI2 (FSPI) is initialised automatically on first call.
 *
 * @return ESP_OK if the controller responded correctly.
 */
esp_err_t bsp_display_probe(void);

/**
 * @brief Configure touch INT and RESET pins; I2C0 must be ready.
 */
esp_err_t bsp_touch_init(void);

/**
 * @brief Probe the GT911 touch controller over I2C0.
 *
 * Reads the product-ID register (0x8140) and logs the result.
 *
 * @return ESP_OK if the controller responded with expected data.
 */
esp_err_t bsp_touch_probe(void);

/**
 * @brief Enable SD-card power and configure CS / card-detect pins.
 */
esp_err_t bsp_sdcard_init(void);

/**
 * @brief Power up the PDM microphone (pwr-en + pin config).
 */
esp_err_t bsp_mic_init(void);

/**
 * @brief Configure the buzzer pin as a push-pull output (idle LOW).
 */
esp_err_t bsp_buzzer_init(void);

/**
 * @brief Configure the battery ADC pin and load switch.
 *
 * The VBAT switch is kept OFF after init; call bsp_battery_read_mv() to
 * take a one-shot measurement.
 */
esp_err_t bsp_battery_init(void);

/**
 * @brief Enable the battery load switch, take an ADC reading, then disable.
 *
 * @param[out] voltage_mv  Battery voltage in millivolts.
 * @return ESP_OK on success.
 */
esp_err_t bsp_battery_read_mv(uint32_t *voltage_mv);

/* ==========================================================================
 * Power management
 * ========================================================================== */

/**
 * @brief Power down all peripherals before entering deep sleep.
 *
 * Turns off display power, SD card, I2C bus, LED, and sets GPIOs to
 * low-leakage state.  Call before esp_deep_sleep_start().
 */
void bsp_power_down(void);

/**
 * @brief Enter deep sleep with timer and/or GPIO wake-up.
 *
 * @param sleep_sec   Wake after this many seconds (0 = no timer wake).
 * @param btn_wake    If true, KEY0 (GPIO3, active LOW) will also wake.
 */
void bsp_deep_sleep_enter(uint32_t sleep_sec, bool btn_wake);

/**
 * @brief Return a human-readable string describing the last wake cause.
 */
const char *bsp_wake_cause_str(void);

/* ==========================================================================
 * Onboard sensors
 * ========================================================================== */

/**
 * @brief RTC time struct (PCF8563).
 */
typedef struct {
    int  year;       /* full year (e.g. 2026) */
    int  month;      /* 1–12 */
    int  day;        /* 1–31 */
    int  hour;       /* 0–23 */
    int  minute;     /* 0–59 */
    int  second;     /* 0–59 */
    bool voltage_ok; /* false = VL flag set, battery drained, time unreliable */
} bsp_rtc_time_t;

/**
 * @brief Set the PCF8563 RTC time.
 *
 * @param[in]  time  New time to write (year in 2000–2099 range).
 * @return ESP_OK, ESP_ERR_NOT_FOUND if RTC is unreachable, or an I2C error.
 */
esp_err_t bsp_rtc_set_time(const bsp_rtc_time_t *time);

/**
 * @brief Read the current time from the PCF8563 RTC.
 *
 * @param[out] time  Populated with the RTC time on success.
 * @return ESP_OK, ESP_ERR_NOT_FOUND if RTC is unreachable, or an I2C error.
 */
esp_err_t bsp_rtc_read_time(bsp_rtc_time_t *time);

/**
 * @brief Read temperature and humidity from the SHT4x sensor.
 *
 * @param[out] temp_c       Temperature in degrees Celsius.
 * @param[out] humidity_pct Relative humidity in percent (0–100).
 * @return ESP_OK, ESP_ERR_NOT_FOUND if sensor is unreachable, or an I2C error.
 */
esp_err_t bsp_sht4x_read(float *temp_c, float *humidity_pct);

#ifdef __cplusplus
}
#endif
