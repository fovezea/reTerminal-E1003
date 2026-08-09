# reTerminal-E1003

Seeed Studio white-paper HMI firmware — ESP-IDF v6.

## Hardware

| Component | Detail |
|-----------|--------|
| **MCU** | ESP32-S3-WROOM-1-N16R8 (16 MB Flash, 8 MB Octal PSRAM, 240 MHz) |
| **Display** | ED103TC2 10.3" e-Paper, 1872×1404, 16-level grayscale |
| **Display controller** | IT8951 (SPI2, VCOM = −1.40 V) |
| **Touch** | GT911 (I2C0, addr 0x14 / 0x5D) |
| **RTC** | PCF8563M/TR (I2C0, addr 0x51) |
| **Temp / humidity** | SHT4x (I2C0, addr 0x44) |
| **Microphone** | PDM (GPIO41 CLK, GPIO42 DATA, GPIO38 PWR) |
| **Storage** | MicroSD (SPI2 shared bus, CS=GPIO14) |
| **UI** | 3 buttons (KEY0–2 on GPIO3–5, active LOW), 1 LED (GPIO16, inverted), buzzer (GPIO45) |
| **Battery** | ADC1_CH3 (GPIO3), load switch GPIO40, voltage divider 2:1 |
| **Serial debug** | UART0 via USB-UART bridge (TX=GPIO43, RX=GPIO44, 115200 8N1) |
| **PMIC** | 0x6B on I2C0 (unidentified — likely charger/fuel gauge) |

### Pin map

#### Display (IT8951, SPI2)
| Signal | GPIO | Notes |
|--------|------|-------|
| SCK | 7 | |
| MOSI | 9 | |
| MISO | 8 | |
| CS | 10 | |
| RST | 12 | |
| BUSY (HRDY) | 13 | |
| EPD_TFT_ENABLE | 11 | active-HIGH (panel power) |
| EPD_ITE_ENABLE  | 21 | active-HIGH (IT8951 logic) |

#### I2C0 bus (SDA=GPIO19, SCL=GPIO20, 400 kHz)
| Device | Address |
|--------|---------|
| GT911 touch | 0x14 (fallback 0x5D) |
| SHT4x sensor | 0x44 |
| PCF8563 RTC | 0x51 |
| PMIC / charger | 0x6B |

#### Buttons, LED, buzzer, mic
| Signal | GPIO | Notes |
|--------|------|-------|
| KEY0 (green) | 3 | active LOW, wake from deep sleep |
| KEY1 | 4 | active LOW |
| KEY2 | 5 | active LOW |
| LED | 16 | inverted: LOW=ON |
| Buzzer | 45 | |
| Mic CLK | 42 | PDM |
| Mic DATA | 41 | PDM |
| Mic PWR | 38 | active HIGH |

#### SD card (SPI2 shared with display)
| Signal | GPIO |
|--------|------|
| SCK | 7 |
| MOSI | 9 |
| MISO | 8 |
| CS | 14 |
| DET | 15 (LOW=inserted) |
| PWR | 39 (active HIGH) |

#### Battery
| Signal | GPIO |
|--------|------|
| ADC | 3 (ADC1_CH3, shared with KEY0) |
| Switch | 40 (HIGH = battery connected) |
| Divider ratio | 2:1 |

### Power enables

Both display enables are **active-HIGH** — confirmed by the working GxEPD2 Arduino sketch
and the Seeed LVGLePaperStatusPanel example:
- GPIO11 (`EPD_TFT_ENABLE`) — set HIGH to turn ON panel power
- GPIO21 (`EPD_ITE_ENABLE`) — set HIGH to turn ON IT8951 logic

The original BSP incorrectly assumed P-channel load switches (active-LOW), which left
the panel power supply OFF and caused HRDY/BUSY to stay stuck LOW.

## Project structure

```
reTerminal-E1003/
├── CMakeLists.txt          # Top-level ESP-IDF project
├── sdkconfig               # Generated kconfig (git-ignored)
├── sdkconfig.defaults      # Kconfig defaults for ESP32-S3 N16R8
├── partitions.csv          # Custom partition table (16 MB flash)
├── main/
│   ├── CMakeLists.txt
│   └── main.c              # Application entry point
├── components/
│   └── bsp/                # Board Support Package
│       ├── CMakeLists.txt
│       ├── include/bsp.h   # Pin defines, init declarations
│       └── bsp.c           # Peripheral init, sensors, power mgmt
└── OSHW-reTerminal/        # Manufacturer examples & schematics
```

## BSP API

```c
// Init
esp_err_t bsp_init(void);            // Full board init (I2C, display, touch, SD, etc.)
esp_err_t bsp_display_init(void);    // Display GPIOs + power enables
esp_err_t bsp_touch_init(void);      // Touch INT/RST pins
esp_err_t bsp_sdcard_init(void);     // SD card power + CS/DET
esp_err_t bsp_mic_init(void);        // PDM mic power
esp_err_t bsp_buzzer_init(void);     // Buzzer pin
esp_err_t bsp_battery_init(void);    // ADC + load switch

// Probes
esp_err_t bsp_display_probe(void);   // IT8951 SPI init + register read
esp_err_t bsp_touch_probe(void);     // GT911 product ID

// Sensors
esp_err_t bsp_rtc_read_time(bsp_rtc_time_t *time);   // PCF8563
esp_err_t bsp_rtc_set_time(const bsp_rtc_time_t *time);
esp_err_t bsp_sht4x_read(float *temp_c, float *humidity_pct);
esp_err_t bsp_battery_read_mv(uint32_t *voltage_mv);

// Power management
void     bsp_power_down(void);                         // Kill all peripherals
void     bsp_deep_sleep_enter(uint32_t sleep_sec, bool btn_wake);
const char *bsp_wake_cause_str(void);                  // "timer", "button", "power-on"

// LED helpers (GPIO16, inverted logic)
#define BSP_LED_ON()    gpio_set_level(GPIO_NUM_16, 0)
#define BSP_LED_OFF()   gpio_set_level(GPIO_NUM_16, 1)
```

## Build

### Prerequisites

ESP-IDF v6 installed via the new EIM (ESP-IDF Installation Manager). Tools are at
`C:\Espressif\tools\`, ESP-IDF at `C:\esp\v6.0.2\esp-idf`.

### Activate environment (PowerShell)

```powershell
. 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1'
```

### Build, flash, monitor

```powershell
cd C:\SRC\reTerminal-E1003
idf.py set-target esp32s3   # first time only
idf.py build
idf.py -p COM3 flash monitor
```

### Initial setup notes

The `sdkconfig` was adapted from the manufacturer's TRMNL firmware (`sdkconfig.TRMNL_X_E1003`)
with Arduino and WiFi disabled. Key differences from vanilla ESP-IDF defaults:

- Console: UART0 (GPIO43/44), not USB Serial/JTAG (GPIO19/20 are used for I2C)
- PSRAM: `CONFIG_SPIRAM_USE_MALLOC=y`, BSS kept in internal RAM
- Flash: 16 MB QIO @ 80 MHz
- Sleep GPIO reset workaround disabled

## Display driver architecture

The IT8951 driver (`components/bsp/it8951.c`) was ported from the GxEPD2
`ED103TC2_1872x1404` reference in `OSHW-reTerminal/examples/`.

### Data paths

There are two IT8951 data paths:

| Path | BPP | Width param | Data per row | UP1SR | Used by |
|------|-----|-------------|--------------|-------|---------|
| **8BPP** (proven) | 1 byte/px | 1872 px | 1872 B | No | `clear_screen`, LVGL `panel_update` |
| **1bpp** (partial) | 1 bit/px | 234 B (packed) | 234 B | Yes | `load_start/flush/end` + `display_area` |

The **8BPP path** is the reliable one for full-screen updates — it expands each
pixel to a byte (0x00 black, 0xFF white) and uses GC16/DU mode without UP1SR.
The 1bpp path works for partial regions (rectangles) but has an unresolved
byte-level alignment issue with full-screen mixed-content data.

### LVGL integration

`lvgl_port.cpp` follows the Seeed `LVGLePaperStatusPanel` example:

1. LVGL renders RGB565 into a small PARTIAL buffer (20 lines × 1872 px)
2. The flush callback converts RGB565 → 8BPP and writes to a full-screen
   framebuffer (2.6 MB in PSRAM): `s_fb[y * W + x] = dark ? 0x00 : 0xFF`
3. After `lv_timer_handler()`, `bsp_lvgl_panel_update()` calls
   `it8951_write_8bpp_frame()` which sends the 8BPP buffer via the proven
   bulk-transfer path with X-mirror byte reversal
4. One DPY_AREA refresh (DU mode, zero flash) updates the panel

### Key fixes discovered

- **GPIO11/GPIO21 polarity**: The enables are active-**HIGH**, matching the
  GxEPD2 Arduino sketch. Setting them LOW (as originally done) leaves the
  panel power OFF, causing HRDY/BUSY to stay stuck LOW.
- **`wait_hrdy()` yields**: Uses `vTaskDelay(1)` instead of `esp_rom_delay_us()`
  to feed the FreeRTOS task watchdog during long SPI operations.
- **Bulk SPI transfers**: `xfer8n()` sends up to 4092 bytes per transaction,
  avoiding the overhead of per-byte `spi_device_polling_transmit()` calls.
- **SD card power OFF during IT8951 init**: Both share SPI2 MISO (GPIO8);
  the SD card must be deselected to avoid bus conflict.
- **Temperature command**: `write_cmd(0x0040)` must be sent before every
  DPY_AREA; without it the IT8951 accepts the command but skips the refresh.

### Known hardware findings

### 0x6B I2C device

A device at I2C address 0x6B responds to probes. Based on common reTerminal designs,
this is likely a battery charger / fuel gauge PMIC (possibly BQ25616 or similar).

### PSRAM detection

`esp_chip_info.features & CHIP_FEATURE_EMB_PSRAM` returns false on ESP32-S3 because
the PSRAM is external (Octal SPI), not embedded. Use `esp_psram_get_size()` instead.

## Current status

| Peripheral | Status |
|-----------|--------|
| PSRAM (8 MB Octal) | ✅ Working |
| Flash (16 MB QIO) | ✅ Working |
| I2C bus + devices | ✅ All 4 found |
| GT911 touch | ✅ Product ID "911" |
| SHT4x sensor | ✅ 28°C, 47% RH |
| PCF8563 RTC | ✅ Read + set + sync |
| Battery ADC | ✅ Reading (needs calibration) |
| Deep sleep (timer) | ✅ 30s cycle |
| Deep sleep (button) | ✅ GPIO3 wake |
| WiFi | ✅ Tested (removed from current build for power) |
| IT8951 display | ✅ Working — 8BPP path with LVGL rendering |
| LVGL on e-Paper | ✅ Working — RGB565→8BPP, DU mode (zero flash) |
