/*
 * it8951.c — IT8951 driver for reTerminal E1003
 *
 * Rewritten to match the WORKING GxEPD2_ED103TC2_1872x1404 driver from
 * Seeed Studio's OSHW-reTerminal repository.
 *
 * ED103TC2 panel, 1872×1404, VCOM = -1.40 V, SPI2 at 10 MHz MODE0.
 */

#include "it8951.h"
#include "bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "it8951";

/* IT8951 commands */
enum {
    CMD_SYS_RUN      = 0x0001,
    CMD_STANDBY      = 0x0002,
    CMD_SLEEP        = 0x0003,
    CMD_REG_RD       = 0x0010,
    CMD_REG_WR       = 0x0011,
    CMD_LD_IMG_AREA  = 0x0021,
    CMD_LD_IMG_END   = 0x0022,
    CMD_DPY_AREA     = 0x0034,
    CMD_GET_DEV_INFO = 0x0302,
    CMD_VCOM         = 0x0039,
};

/* IT8951 registers */
enum {
    REG_LISAR    = 0x0208,  /* image buffer base address */
    REG_UP1SR    = 0x1138,  /* update parameter 1 setting */
    REG_LUTAFSR  = 0x1224,  /* LUT engine status */
    REG_BGVR     = 0x1250,  /* background / foreground */
};

/* Preambles */
#define PRE_WRITE_CMD  0x6000
#define PRE_WRITE_DATA 0x0000
#define PRE_READ_DATA  0x1000

/* Panel */
#define PANEL_W  1872
#define PANEL_H  1404
#define VCOM_MV  1400
#define IMG_BUF  0x0012E000

/* Row width in bytes for 1bpp framebuffer */
#define WB  ((PANEL_W + 7) / 8)

static spi_device_handle_t s_spi;
static uint16_t s_panel_w, s_panel_h;
static uint32_t s_img_buf_addr = IMG_BUF;

/* ========================================================================== */
static void cs_low(void)  { gpio_set_level(BSP_DISP_CS, 0); }
static void cs_high(void) { gpio_set_level(BSP_DISP_CS, 1); }

/* Wait for LUT engines to go idle — matches GxEPD2 _waitForLUT() */
static void wait_lut(void);

/* Wait for HRDY with RTOS yield — matches GxEPD2 _waitForHRDY().
 * Without yield() the task watchdog fires during long transfers. */
static void wait_hrdy(void)
{
    int64_t start = esp_timer_get_time() / 1000; /* ms */
    while (gpio_get_level(BSP_DISP_BUSY) == 0) {
        if ((esp_timer_get_time() / 1000) - start > 2000) {
            ESP_LOGW(TAG, "HRDY timeout");
            break;
        }
        vTaskDelay(1);   /* feeds the watchdog */
    }
}

static uint16_t xfer16(uint16_t v) {
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .length = 16,
        .tx_data = { (uint8_t)(v >> 8), (uint8_t)(v & 0xFF) },
    };
    spi_device_polling_transmit(s_spi, &t);
    return ((uint16_t)t.rx_data[0] << 8) | t.rx_data[1];
}

/* Send a single byte via SPI (used during pixel-data streaming) */
static void xfer8(uint8_t v) {
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA,
        .length = 8,
        .tx_data = { v },
    };
    spi_device_polling_transmit(s_spi, &t);
}

/* ---- Protocol primitives (match GxEPD2 exactly) ---- */

static void write_cmd(uint16_t cmd) {
    cs_low(); wait_hrdy(); xfer16(PRE_WRITE_CMD); wait_hrdy(); xfer16(cmd); cs_high();
}
static void write_data(uint16_t d) {
    cs_low(); wait_hrdy(); xfer16(PRE_WRITE_DATA); wait_hrdy(); xfer16(d); cs_high();
}
static uint16_t read_data(void) {
    cs_low(); wait_hrdy(); xfer16(PRE_READ_DATA); wait_hrdy();
    (void)xfer16(0); wait_hrdy(); uint16_t v = xfer16(0); cs_high(); return v;
}
static uint16_t read_reg(uint16_t reg) { write_cmd(CMD_REG_RD); write_data(reg); return read_data(); }
static void write_reg(uint16_t reg, uint16_t val) { write_cmd(CMD_REG_WR); write_data(reg); write_data(val); }

/* Wait for LUT engines to go idle — matches GxEPD2 _waitForLUT() */
static void wait_lut(void)
{
    int64_t start = esp_timer_get_time() / 1000;
    while (1) {
        uint16_t v = read_reg(REG_LUTAFSR);
        if (v == 0) break;
        if ((esp_timer_get_time() / 1000) - start > 30000) {
            ESP_LOGW(TAG, "LUT busy timeout");
            break;
        }
        vTaskDelay(10);
    }
}

/* ==========================================================================
 * Image buffer address — set before every image load (GxEPD2)
 * ========================================================================== */
static void set_img_buf_addr(void) {
    uint16_t hi = (uint16_t)((s_img_buf_addr >> 16) & 0xFFFF);
    uint16_t lo = (uint16_t)(s_img_buf_addr & 0xFFFF);
    write_reg(REG_LISAR + 2, hi);
    write_reg(REG_LISAR,     lo);
}

/* ==========================================================================
 * Init
 * ========================================================================== */
static void hw_reset(void) {
    gpio_set_level(BSP_DISP_RST, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BSP_DISP_RST, 1); vTaskDelay(pdMS_TO_TICKS(10));
    wait_hrdy();
}

static void set_vcom(uint16_t mv) {
    write_cmd(CMD_VCOM); write_data(0x0002); write_data(mv);
    ESP_LOGI(TAG, "VCOM = -%.2f V", (float)mv / 1000.0f);
}

static void read_dev_info(void) {
    write_cmd(CMD_GET_DEV_INFO);
    uint16_t info[20];
    /* Single CS session: preamble → dummy → 20 data words (GxEPD2 _readData16N) */
    cs_low(); wait_hrdy(); xfer16(PRE_READ_DATA); wait_hrdy();
    (void)xfer16(0); wait_hrdy();
    for (int i = 0; i < 20; i++) info[i] = xfer16(0);
    cs_high();

    s_panel_w = info[0]; s_panel_h = info[1];
    s_img_buf_addr = (uint32_t)info[2] | ((uint32_t)info[3] << 16);

    char fw[17] = {0};
    for (int i = 0; i < 8; i++) { fw[i*2] = info[4+i] & 0xFF; fw[i*2+1] = info[4+i] >> 8; }
    if (s_panel_w == 0xFFFF || s_panel_w == 0) { s_panel_w = PANEL_W; s_panel_h = PANEL_H; }
    ESP_LOGI(TAG, "Panel: %ux%u  FW: %s", s_panel_w, s_panel_h, fw);
}

esp_err_t it8951_init(void) {
    /* SD card shares SPI2 MISO — power it OFF to avoid bus conflict */
    gpio_set_level(BSP_SD_PWR_EN, 0);
    gpio_set_level(BSP_SD_CS, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* GxEPD2 Arduino sketch sets both ENABLE pins HIGH.
     * BSP comment said "active-low P-channel" but working code proves active-HIGH. */
    gpio_set_level(BSP_DISP_DC, 1);     /* GPIO11 = EPD_TFT_ENABLE = HIGH */
    gpio_set_level(BSP_DISP_VCC_EN, 1); /* GPIO21 = EPD_ITE_ENABLE = HIGH */
    vTaskDelay(pdMS_TO_TICKS(100));

    spi_bus_config_t bus = { .mosi_io_num = 9, .miso_io_num = 8, .sclk_io_num = 7,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4096 };
    spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    spi_device_interface_config_t dev = { .mode = 0, .clock_speed_hz = 10*1000*1000,
        .spics_io_num = -1, .queue_size = 1 };
    spi_bus_add_device(SPI2_HOST, &dev, &s_spi);
    cs_high();
    ESP_LOGI(TAG, "SPI init OK");

    /* Init sequence — matches GxEPD2 exactly */
    hw_reset();
    hw_reset();  /* double reset like GxEPD2 */
    wait_hrdy();
    set_vcom(VCOM_MV);
    read_dev_info();

    /* Temperature must be set — without this IT8951 skips refresh (GxEPD2) */
    write_cmd(0x0040); write_data(0x0001); write_data(16);
    ESP_LOGI(TAG, "IT8951 ready");
    return ESP_OK;
}

/* ==========================================================================
 * Bulk byte-streaming helper — sends len bytes as one SPI transaction.
 * Avoids per-byte xfer8() overhead (each xfer8 is a full SPI transaction).
 * ========================================================================== */
static void xfer8n(const uint8_t *data, size_t len)
{
    /* max_transfer_sz = 4096; use 4092-byte chunks */
    while (len > 0) {
        size_t n = len > 4092 ? 4092 : len;
        spi_transaction_t t = {
            .length    = (int)(n * 8),
            .tx_buffer = data,
        };
        spi_device_polling_transmit(s_spi, &t);
        data += n;
        len  -= n;
    }
}

/* ==========================================================================
 * Full refresh — use 1bpp compact path (~328 KB instead of ~2.6 MB 8BPP)
 * ========================================================================== */

void it8951_clear_screen(void) {
    int64_t t0 = esp_timer_get_time() / 1000;

    /* Temperature must be set before every display operation (GxEPD2) */
    write_cmd(0x0040); write_data(0x0001); write_data(16);

    set_img_buf_addr();

    /* LD_IMG_AREA: 1bpp path (compact), L_ENDIAN */
    {
        uint16_t args[5];
        args[0] = (0 << 8) | (3 << 4) | 0;        /* L_ENDIAN, 8BPP, ROTATE_0 */
        args[1] = 0;                               /* x in bytes */
        args[2] = 0;
        args[3] = WB;                              /* width in bytes */
        args[4] = PANEL_H;
        write_cmd(CMD_LD_IMG_AREA);
        for (int i = 0; i < 5; i++) write_data(args[i]);
    }

    /* All-white pixel data using 1bpp compact path */
    static uint8_t white_row[WB];
    static bool row_init = false;
    if (!row_init) { memset(white_row, 0xFF, WB); row_init = true; }

    cs_low(); wait_hrdy(); xfer16(PRE_WRITE_DATA); wait_hrdy();

    for (uint16_t row = 0; row < PANEL_H; row++) {
        xfer8n(white_row, WB);
        if ((row & 0x3F) == 0) vTaskDelay(1);  /* feed watchdog */
    }
    cs_high();

    write_cmd(CMD_LD_IMG_END); wait_hrdy();

    /* Enable 1bpp mode for DPY_AREA — GxEPD2 _dpy_area_1bpp() */
    {
        uint16_t up1sr_hi = read_reg(REG_UP1SR + 2);
        write_reg(REG_UP1SR + 2, up1sr_hi | (1u << 2));
        write_reg(REG_BGVR, ((uint16_t)0x00 << 8) | 0xFF);  /* bit0=black, bit1=white */
    }

    write_cmd(CMD_DPY_AREA);
    write_data(0); write_data(0);
    write_data(PANEL_W); write_data(PANEL_H);
    write_data(1);  /* DU — direct update, zero flash */
    wait_hrdy();

    wait_lut();

    /* Restore normal mode */
    {
        uint16_t up1sr_hi = read_reg(REG_UP1SR + 2);
        write_reg(REG_UP1SR + 2, up1sr_hi & ~(1u << 2));
    }

    /* Wait for physical refresh to complete */
    int64_t t1 = esp_timer_get_time() / 1000;
    while (gpio_get_level(BSP_DISP_BUSY) == 0) {
        if ((esp_timer_get_time() / 1000) - t1 > 15000) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Screen cleared in %lld ms", (long long)(esp_timer_get_time() / 1000 - t0));
}

/* Deep-clean with INIT mode — flashes but removes all ghosting.
 * Use once at boot; subsequent updates use clear_screen (DU mode). */
void it8951_clean_screen(void) {
    int64_t t0 = esp_timer_get_time() / 1000;

    write_cmd(0x0040); write_data(0x0001); write_data(16);

    set_img_buf_addr();

    static uint8_t white_row[WB];
    static bool row_init = false;
    if (!row_init) { memset(white_row, 0xFF, WB); row_init = true; }

    /* 1bpp image load — white */
    {
        uint16_t args[5];
        args[0] = (0 << 8) | (3 << 4) | 0;        /* L_ENDIAN, 8BPP, ROTATE_0 */
        args[1] = 0; args[2] = 0;
        args[3] = WB; args[4] = PANEL_H;
        write_cmd(CMD_LD_IMG_AREA);
        for (int i = 0; i < 5; i++) write_data(args[i]);
    }
    cs_low(); wait_hrdy(); xfer16(PRE_WRITE_DATA); wait_hrdy();
    for (uint16_t row = 0; row < PANEL_H; row++) {
        xfer8n(white_row, WB);
        if ((row & 0x3F) == 0) vTaskDelay(1);
    }
    cs_high();
    write_cmd(CMD_LD_IMG_END); wait_hrdy();

    /* DPY_AREA with INIT mode (0) — full deep clean */
    {
        uint16_t up1sr_hi = read_reg(REG_UP1SR + 2);
        write_reg(REG_UP1SR + 2, up1sr_hi | (1u << 2));
        write_reg(REG_BGVR, ((uint16_t)0x00 << 8) | 0xFF);
    }
    write_cmd(CMD_DPY_AREA);
    write_data(0); write_data(0);
    write_data(PANEL_W); write_data(PANEL_H);
    write_data(0);  /* INIT */
    wait_hrdy();
    wait_lut();
    {
        uint16_t up1sr_hi = read_reg(REG_UP1SR + 2);
        write_reg(REG_UP1SR + 2, up1sr_hi & ~(1u << 2));
    }

    ESP_LOGI(TAG, "Deep-clean done in %lld ms", (long long)(esp_timer_get_time() / 1000 - t0));
}

/* ==========================================================================
 * 1bpp partial image load — matches GxEPD2 _dumpFrameToIT8951() +
 * _ld_img_area_1bpp() + _dpy_area_1bpp()
 * ========================================================================== */

void it8951_load_start(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    /* X-flip (GxEPD2 _ld_img_area_1bpp) */
    uint16_t x_flip = (PANEL_W - 1) - x - w + 1;

    set_img_buf_addr();

    uint16_t args[5];
    args[0] = (0 << 8) | (3 << 4) | 0;        /* L_ENDIAN, 8BPP, ROTATE_0 */
    args[1] = (x_flip + 7) / 8;                /* x in 8-pixel units */
    args[2] = y;
    args[3] = (w + 7) / 8;                     /* width in 8-pixel units */
    args[4] = h;
    write_cmd(CMD_LD_IMG_AREA);
    for (int i = 0; i < 5; i++) write_data(args[i]);

    /* One 0x0000 preamble, then raw bytes follow (caller streams via load_flush) */
    cs_low(); wait_hrdy(); xfer16(PRE_WRITE_DATA); wait_hrdy();
}
void it8951_load_flush(const uint8_t *data, size_t len) {
    /* Stream raw 1bpp bytes as one bulk SPI transaction per call.
     * X-mirror: caller must reverse byte order within each row. */
    xfer8n(data, len);
}
void it8951_load_end(void) {
    cs_high();
    write_cmd(CMD_LD_IMG_END); wait_hrdy();
}

void it8951_display_area(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    /* Temperature must be set before DPY_AREA — without it IT8951 may skip
     * the refresh (GxEPD2 _doFullRefresh always calls this first). */
    write_cmd(0x0040); write_data(0x0001); write_data(16);

    uint16_t x_flip = (PANEL_W - 1) - x - w + 1;

    /* Enable 1bpp mode — GxEPD2 _dpy_area_1bpp() */
    uint16_t up1sr_hi = read_reg(REG_UP1SR + 2);
    write_reg(REG_UP1SR + 2, up1sr_hi | (1u << 2));

    /* Background = 0xFF (white/1), Foreground = 0x00 (black/0) */
    write_reg(REG_BGVR, ((uint16_t)0x00 << 8) | 0xFF);  /* bit0=black, bit1=white */

    write_cmd(CMD_DPY_AREA);
    write_data(x_flip);
    write_data(y);
    write_data(w);
    write_data(h);
    write_data(1);  /* DU — direct update, zero flash */
    wait_hrdy();

    wait_lut();

    /* Restore normal mode */
    up1sr_hi = read_reg(REG_UP1SR + 2);
    write_reg(REG_UP1SR + 2, up1sr_hi & ~(1u << 2));
}

/* ==========================================================================
 * 8BPP full-frame — GxEPD2 _doFullRefresh clone (proven working path)
 * ========================================================================== */
void it8951_write_8bpp_frame(const uint8_t *fb8)
{
    int64_t t0 = esp_timer_get_time() / 1000;

    write_cmd(0x0040); write_data(0x0001); write_data(16);

    /* LD_IMG_AREA: B_ENDIAN, 8BPP, full pixel width */
    {
        uint16_t args[5];
        args[0] = (1 << 8) | (3 << 4) | 0;  /* B_ENDIAN, 8BPP, ROTATE_0 */
        args[1] = 0;
        args[2] = 0;
        args[3] = PANEL_W;    /* width in PIXELS */
        args[4] = PANEL_H;
        write_cmd(CMD_LD_IMG_AREA);
        for (int i = 0; i < 5; i++) write_data(args[i]);
    }

    /* One 0x0000 preamble, then stream 8BPP bytes in bulk.
     * X-mirror: send each row right-to-left. */
    cs_low(); wait_hrdy(); xfer16(PRE_WRITE_DATA); wait_hrdy();

    /* Row buffer for X-mirrored row (internal SRAM, DMA-safe) */
    uint8_t *mirror_row = malloc(PANEL_W);
    assert(mirror_row);

    for (uint16_t row = 0; row < PANEL_H; row++) {
        const uint8_t *rp = fb8 + (uint32_t)row * PANEL_W;
        /* X-mirror: reverse bytes within row */
        for (int32_t col = 0; col < PANEL_W; col++) {
            mirror_row[col] = rp[PANEL_W - 1 - col];
        }
        xfer8n(mirror_row, PANEL_W);
        if ((row & 0x3F) == 0) vTaskDelay(1);
    }
    free(mirror_row);
    cs_high();

    write_cmd(CMD_LD_IMG_END); wait_hrdy();

    /* DPY_AREA — GC16, no UP1SR needed for 8BPP */
    write_cmd(CMD_DPY_AREA);
    write_data(0); write_data(0);
    write_data(PANEL_W); write_data(PANEL_H);
    write_data(2);  /* GC16 */
    wait_hrdy();

    /* Wait for refresh to complete */
    int64_t t1 = esp_timer_get_time() / 1000;
    while (gpio_get_level(BSP_DISP_BUSY) == 0) {
        if ((esp_timer_get_time() / 1000) - t1 > 15000) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "8BPP frame: %lld ms", (long long)(esp_timer_get_time() / 1000 - t0));
}

uint16_t it8951_get_width(void)  { return s_panel_w; }
uint16_t it8951_get_height(void) { return s_panel_h; }
