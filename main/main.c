#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "bsp.h"
#include "it8951.h"
#include "lvgl.h"

static const char *TAG = "app";

/* Li-Po battery: 4.20V = 100%, 3.20V = 0% */
static int battery_pct(uint32_t mv)
{
    if (mv >= 4200) return 100;
    if (mv <= 3200) return 0;
    return (int)((mv - 3200) * 100 / 1000);
}

/* Day-of-week: Zeller's congruence (0=Sun..6=Sat) */
static int day_of_week(int d, int m, int y)
{
    if (m < 3) { m += 12; y--; }
    int K = y % 100, J = y / 100;
    return (d + (13*(m+1))/5 + K + K/4 + J/4 + 5*J) % 7;
}

/* ── Battery bar: outline + fill + terminal tab + percentage ── */
static void create_battery_bar(lv_obj_t *parent, int pct, int x, int y,
                                lv_obj_t **pct_label_out)
{
    /* Outline */
    lv_obj_t *outline = lv_obj_create(parent);
    lv_obj_set_pos(outline, x, y);
    lv_obj_set_size(outline, 220, 56);
    lv_obj_set_style_bg_color(outline, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(outline, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(outline, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(outline, 3, 0);
    lv_obj_set_style_radius(outline, 8, 0);
    lv_obj_set_style_pad_all(outline, 0, 0);

    /* Terminal tab */
    lv_obj_t *tab = lv_obj_create(parent);
    lv_obj_set_pos(tab, x + 220, y + 12);
    lv_obj_set_size(tab, 14, 32);
    lv_obj_set_style_bg_color(tab, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tab, 0, 0);
    lv_obj_set_style_radius(tab, 4, 0);

    /* Fill — colour darkens as battery drops */
    int fill_w = pct * 208 / 100;
    if (fill_w < 8) fill_w = 8;

    uint32_t fill_colour;
    if (pct > 50)      fill_colour = 0x333333;  /* dark gray — healthy */
    else if (pct > 20) fill_colour = 0x666666;  /* mid gray — warning */
    else                fill_colour = 0x999999;  /* light gray — critical */

    lv_obj_t *fill = lv_obj_create(parent);
    lv_obj_set_pos(fill, x + 6, y + 6);
    lv_obj_set_size(fill, fill_w, 44);
    lv_obj_set_style_bg_color(fill, lv_color_hex(fill_colour), 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(fill, 0, 0);
    lv_obj_set_style_radius(fill, 4, 0);

    /* Percentage label next to bar */
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_obj_t *pct_lbl = lv_label_create(parent);
    lv_label_set_text(pct_lbl, buf);
    lv_obj_set_style_text_font(pct_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(pct_lbl, lv_color_black(), 0);
    lv_obj_set_pos(pct_lbl, x + 250, y + 10);
    if (pct_label_out) *pct_label_out = pct_lbl;
}

/* ── Sensor card with border, light background ── */
static lv_obj_t *create_card(lv_obj_t *parent, const char *title,
                              const char *value, int x, int y,
                              int w, int h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 0, 0);

    /* Value — large, bold */
    lv_obj_t *val_lbl = lv_label_create(card);
    lv_label_set_text(val_lbl, value);
    lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(val_lbl, lv_color_black(), 0);
    lv_obj_align(val_lbl, LV_ALIGN_CENTER, 0, -16);

    /* Title — small, gray, below value */
    lv_obj_t *title_lbl = lv_label_create(card);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0x666666), 0);
    lv_obj_align(title_lbl, LV_ALIGN_CENTER, 0, 28);

    return card;
}

/* ══════════════════════════════════════════════════════════════════════
 * Main
 * ══════════════════════════════════════════════════════════════════════ */
void app_main(void)
{
    const char *wake = bsp_wake_cause_str();
    bool cold_boot = (strcmp(wake, "power-on / reset") == 0);
    ESP_LOGI(TAG, "reTerminal E1003 — LVGL Dashboard  [wake: %s]", wake);
    ESP_LOGI(TAG, "PSRAM: %u MB", (unsigned)(esp_psram_get_size() >> 20));

    /* ── Board & sensors ── */
    bsp_init();

    uint32_t bat_mv;
    bsp_battery_read_mv(&bat_mv);
    ESP_LOGI(TAG, "Battery: %lu mV", (unsigned long)bat_mv);

    float temp_c = 0, humidity = 0;
    bsp_sht4x_read(&temp_c, &humidity);
    ESP_LOGI(TAG, "SHT4x: %.1f C, %.0f %%", temp_c, humidity);

    bsp_rtc_time_t rtc;
    bool rtc_ok = (bsp_rtc_read_time(&rtc) == ESP_OK);
    /* Reject clearly-unset RTC (year outside 2024-2099) */
    if (rtc_ok && (rtc.year < 2024 || rtc.year > 2099)) rtc_ok = false;
    if (rtc_ok)
        ESP_LOGI(TAG, "RTC: %04d-%02d-%02d %02d:%02d:%02d  VL=%d",
                 rtc.year, rtc.month, rtc.day,
                 rtc.hour, rtc.minute, rtc.second, !rtc.voltage_ok);

    /* ── Display & LVGL ── */
    ESP_ERROR_CHECK(bsp_lvgl_display_init());
    ESP_ERROR_CHECK(bsp_lvgl_touch_init());
    ESP_ERROR_CHECK(bsp_lvgl_tick_init());

    if (cold_boot) it8951_clean_screen();

    /* ── Build UI ── */
    int pct = battery_pct(bat_mv);
    char buf[128];

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* ── Header ── */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "reTerminal E1003");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 50, 25);

    /* Battery bar on right side of header */
    create_battery_bar(scr, pct, BSP_LCD_WIDTH - 310, 15, NULL);

    /* Separator line (full width thin rectangle) */
    lv_obj_t *sep = lv_obj_create(scr);
    lv_obj_set_pos(sep, 50, 90);
    lv_obj_set_size(sep, BSP_LCD_WIDTH - 100, 2);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);

    /* ── Sensor cards (3 across) ── */
    int card_w = 520, card_h = 140, gap = 55;
    int total_w = 3 * card_w + 2 * gap;
    int card_x0 = (BSP_LCD_WIDTH - total_w) / 2;
    int card_y = 140;

    /* Temperature */
    snprintf(buf, sizeof(buf), "%.1f °C", temp_c);
    create_card(scr, "Temperature", buf,
                card_x0, card_y, card_w, card_h);

    /* Humidity */
    snprintf(buf, sizeof(buf), "%.0f %% RH", humidity);
    create_card(scr, "Humidity", buf,
                card_x0 + card_w + gap, card_y, card_w, card_h);

    /* Time (RTC or placeholder) */
    if (rtc_ok)
        snprintf(buf, sizeof(buf), "%02d:%02d", rtc.hour, rtc.minute);
    else
        snprintf(buf, sizeof(buf), "--:--");
    create_card(scr, "Time", buf,
                card_x0 + 2 * (card_w + gap), card_y, card_w, card_h);

    /* ── Second row: two wider cards ── */
    int wide_w = (2 * card_w + gap);
    int row2_y = card_y + card_h + 40;

    /* Date card */
    if (rtc_ok) {
        const char *months[] = {"","Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};
        const char *days[]  = {"Sunday","Monday","Tuesday","Wednesday",
                                "Thursday","Friday","Saturday"};
        int dow = day_of_week(rtc.day, rtc.month, rtc.year);
        snprintf(buf, sizeof(buf), "%s, %s %d, %d",
                 days[dow], months[rtc.month], rtc.day, rtc.year);
    } else {
        snprintf(buf, sizeof(buf), "RTC not set");
    }
    create_card(scr, "Date", buf,
                card_x0, row2_y, wide_w, card_h);

    /* Battery mV card */
    snprintf(buf, sizeof(buf), "%lu mV", (unsigned long)bat_mv);
    create_card(scr, "Battery Voltage", buf,
                card_x0 + wide_w + gap, row2_y, card_w, card_h);

    /* ── RTC voltage-low warning ── */
    if (rtc_ok && !rtc.voltage_ok) {
        lv_obj_t *warn = lv_label_create(scr);
        lv_label_set_text(warn, "RTC battery low — time may be unreliable");
        lv_obj_set_style_text_font(warn, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(warn, lv_color_hex(0x888888), 0);
        lv_obj_align(warn, LV_ALIGN_BOTTOM_MID, 0, -20);
    }

    /* ── Render ── */
    ESP_LOGI(TAG, "Rendering...");
    lv_tick_inc(5);
    lv_timer_handler();
    bsp_lvgl_panel_update();
    ESP_LOGI(TAG, "Done — %d battery%%", pct);

    vTaskDelay(pdMS_TO_TICKS(30000));
    bsp_deep_sleep_enter(30, true);
}
