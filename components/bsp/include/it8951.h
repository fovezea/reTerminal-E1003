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

/** Clear the screen to white (INIT mode refresh). */
void it8951_clear_screen(void);

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
