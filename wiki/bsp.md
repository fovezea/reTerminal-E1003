# reTerminal E1003 — BSP Pin Reference

MCU: ESP32-S3-WROOM-1-N16R8 (16 MB Flash, 8 MB Octal PSRAM, 240 MHz)
Display: ED103TC2 10.3" e-Paper, 1872×1404, IT8951 controller

## Display — IT8951 (SPI2 / FSPI)

| Signal | GPIO | Notes |
|--------|------|-------|
| SCK | 7 | SPI clock, 10 MHz, MODE0 |
| MOSI | 9 | |
| MISO | 8 | Shared with SD card |
| CS | 10 | Manual CS (IT8951 preamble protocol) |
| RST | 12 | Active-LOW reset |
| BUSY (HRDY) | 13 | LOW = busy, HIGH = ready |
| EPD_TFT_ENABLE | 11 | Panel power, active-HIGH |
| EPD_ITE_ENABLE | 21 | IT8951 logic power, active-HIGH |

The IT8951 uses a preamble-based command/data protocol — there is no
traditional SPI DC pin. GPIO11 and GPIO21 are power enables, not data/command.

## Touch — GT911 (I2C0)

| Signal | GPIO | Notes |
|--------|------|-------|
| SDA | 19 | Shared I2C0 bus |
| SCL | 20 | Shared I2C0 bus |
| INT | 2 | Active-LOW interrupt |
| RST | 48 | Active-LOW reset |
| Address | 0x14 | Fallback 0x5D |

## I2C0 bus (SDA=GPIO19, SCL=GPIO20, 400 kHz)

| Device | Address | Notes |
|--------|---------|-------|
| GT911 touch | 0x14 | Primary address |
| SHT4x sensor | 0x44 | Temperature + humidity |
| PCF8563 RTC | 0x51 | Real-time clock |
| PMIC / charger | 0x6B | Unidentified |

## User I/O

| Signal | GPIO | Notes |
|--------|------|-------|
| LED | 16 | Inverted: LOW=ON, HIGH=OFF |
| Buzzer | 45 | Active HIGH |
| KEY0 (green button) | 3 | Active LOW, internal pull-up, wake from deep sleep |
| KEY1 (right white) | 4 | Active LOW, internal pull-up |
| KEY2 (left white) | 5 | Active LOW, internal pull-up |

## Battery monitoring

| Signal | GPIO | Notes |
|--------|------|-------|
| ADC | 1 | ADC1_CH0, 12-bit, 12 dB attenuation |
| Enable | 40 | HIGH = battery connected to ADC |
| Divider ratio | 2:1 | `Vbat = Vpin × 2` |

Flow (matching Seeed Arduino example):
1. Set GPIO40 HIGH to enable the monitoring circuit
2. Read ADC1_CH0 (GPIO1)
3. Set GPIO40 LOW to disable
4. Multiply reading by 2 for actual battery voltage

Fully charged LiPo → ~4.2 V (pin ~2.1 V). Low battery → ~3.3 V (pin ~1.65 V).

## PDM microphone

| Signal | GPIO | Notes |
|--------|------|-------|
| CLK | 42 | |
| DATA | 41 | |
| PWR | 38 | Active HIGH |

## MicroSD card (SPI2 shared with display)

| Signal | GPIO | Notes |
|--------|------|-------|
| SCK | 7 | Shared with display |
| MOSI | 9 | Shared with display |
| MISO | 8 | Shared with display |
| CS | 14 | |
| DET | 15 | LOW = card present |
| PWR | 39 | Active HIGH |

SD power must be OFF during IT8951 init to avoid MISO bus conflict.

## Serial debug (UART0)

| Signal | GPIO | Notes |
|--------|------|-------|
| TX | 43 | 115200 8N1 |
| RX | 44 | |

The reTerminal E1003 uses an external USB-UART bridge. The ESP32-S3's
internal USB Serial/JTAG pins are used for I2C0.

## Power enables

Both display enables are **active-HIGH** (confirmed by the working
GxEPD2 Arduino sketch and Seeed LVGLePaperStatusPanel example):

| GPIO | Function | ON | OFF |
|------|----------|----|-----|
| 11 | EPD_TFT_ENABLE (panel) | HIGH | LOW |
| 21 | EPD_ITE_ENABLE (IT8951 logic) | HIGH | LOW |

## BSP API

```c
esp_err_t bsp_init(void);                // Full board init
esp_err_t bsp_display_init(void);        // Display GPIO setup
esp_err_t bsp_touch_init(void);          // Touch GPIO setup
esp_err_t bsp_sdcard_init(void);         // SD card GPIO setup
esp_err_t bsp_mic_init(void);            // PDM mic init
esp_err_t bsp_buzzer_init(void);         // Buzzer pin config
esp_err_t bsp_battery_init(void);        // ADC + calibration setup
esp_err_t bsp_battery_read_mv(uint32_t *mv); // Battery voltage in mV
void     bsp_power_down(void);           // Power off all peripherals
void     bsp_deep_sleep_enter(uint32_t sec, bool btn_wake);
const char *bsp_wake_cause_str(void);

// I2C
i2c_master_bus_handle_t bsp_i2c0_get_handle(void);

// RTC (PCF8563)
esp_err_t bsp_rtc_set_time(const bsp_rtc_time_t *time);
esp_err_t bsp_rtc_read_time(bsp_rtc_time_t *time);

// Sensor (SHT4x)
esp_err_t bsp_sht4x_read(float *temp_c, float *humidity_pct);

// LVGL (lvgl_port.cpp)
esp_err_t bsp_lvgl_display_init(void);
esp_err_t bsp_lvgl_touch_init(void);
void      bsp_lvgl_touch_deinit(void);
esp_err_t bsp_lvgl_tick_init(void);
void      bsp_lvgl_panel_update(void);
```
