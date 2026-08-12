/*
 * it8951.h — Minimal IT8951 driver for reTerminal E1003
 *
 * Ported from the working GxEPD2_ED103TC2_1872x1404 Arduino driver.
 * SPI2 (FSPI) at 10 MHz, MODE0, 1872×1404 ED103TC2 panel, VCOM = -1.40 V.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Full initialisation: SPI, reset, VCOM, temperature, system info. */
esp_err_t it8951_init(void);

/** Clear the screen to white (DU mode — no flash, minimal ghosting). */
void it8951_clear_screen(void);

/** Deep-clean the screen with INIT mode — flashes but removes all ghosting.
 *  Use once at boot; subsequent updates should use clear_screen(). */
void it8951_clean_screen(void);

/**
 * @brief Write a full 8BPP framebuffer and refresh (proven path).
 * @param fb8  Full-screen buffer: WIDTH × HEIGHT bytes.  0x00=black, 0xFF=white.
 */
void it8951_write_8bpp_frame(const uint8_t *fb8);

/**
 * @brief Write a full 4BPP grayscale framebuffer (ESPHome path).
 *
 * Packs 2 pixels/byte from the same 8BPP input: high nibble = even x.
 * 4bpp data: 0x0 = black … 0xF = white.
 * LD_IMG_AREA: L_ENDIAN, 4BPP, full pixel width.
 * Half the SPI traffic of the 8BPP path (~1.3 MB vs 2.6 MB).
 *
 * @param fb8  Full-screen buffer: WIDTH × HEIGHT bytes.  0x00=black, 0xFF=white.
 */
void it8951_write_4bpp_frame(const uint8_t *fb8);
void it8951_write_4bpp_packed(const uint8_t *fb4);  /* pre-packed 4bpp */

/**
 * @brief Write 1bpp image data to the IT8951 image buffer.
 *
 * Call it8951_load_start() first, then this one or more times,
 * then it8951_load_end(), then it8951_display_area().
 *
 * @param data  1-bit-per-pixel bitmap (8 pixels per byte, MSB first).
 * @param x, y  Top-left corner in pixels.
 * @param w, h  Width and height in pixels (w must be multiple of 8).
 */
void it8951_load_start(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void it8951_load_flush(const uint8_t *data, size_t len);
void it8951_load_end(void);

/** Refresh a region of the display (INIT = full, A2 = fast, GC16 = 16-gray). */
void it8951_display_area(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/** Get framebuffer width/height after init. */
uint16_t it8951_get_width(void);
uint16_t it8951_get_height(void);

#ifdef __cplusplus
}
#endif
