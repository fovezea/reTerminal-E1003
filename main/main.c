#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_sntp.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "bsp.h"
#include "it8951.h"
#include "wifi_manager.h"
#include "lvgl.h"

static const char *TAG = "app";

/* WiFi provisioning portal budget — stay awake (no deep sleep) this long
 * while waiting for the user to configure, then give up and continue. */
#define PORTAL_TIMEOUT_MS   (300 * 1000)
#define WIFI_CONNECT_MS     15000

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

/* ── LVGL helpers ─────────────────────────────────────────────────────── */

/* Push the current LVGL screen to the e-paper with one refresh. */
static void render_now(void)
{
    lv_tick_inc(5);
    lv_timer_handler();
    bsp_lvgl_panel_update();
}

static void set_status(lv_obj_t *lbl, const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(lbl, buf);
}

/* ── Battery bar: outline + fill + terminal tab + percentage ── */
static void create_battery_bar(lv_obj_t *parent, int pct, int x, int y,
                                lv_obj_t **pct_label_out)
{
    lv_obj_t *outline = lv_obj_create(parent);
    lv_obj_set_pos(outline, x, y);
    lv_obj_set_size(outline, 220, 56);
    lv_obj_set_style_bg_color(outline, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(outline, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(outline, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(outline, 3, 0);
    lv_obj_set_style_radius(outline, 8, 0);
    lv_obj_set_style_pad_all(outline, 0, 0);

    lv_obj_t *tab = lv_obj_create(parent);
    lv_obj_set_pos(tab, x + 220, y + 12);
    lv_obj_set_size(tab, 14, 32);
    lv_obj_set_style_bg_color(tab, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tab, 0, 0);
    lv_obj_set_style_radius(tab, 4, 0);

    int fill_w = pct * 208 / 100;
    if (fill_w < 8) fill_w = 8;

    uint32_t fill_colour;
    if (pct > 50)      fill_colour = 0x333333;
    else if (pct > 20) fill_colour = 0x666666;
    else                fill_colour = 0x999999;

    lv_obj_t *fill = lv_obj_create(parent);
    lv_obj_set_pos(fill, x + 6, y + 6);
    lv_obj_set_size(fill, fill_w, 44);
    lv_obj_set_style_bg_color(fill, lv_color_hex(fill_colour), 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(fill, 0, 0);
    lv_obj_set_style_radius(fill, 4, 0);

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

    lv_obj_t *val_lbl = lv_label_create(card);
    lv_label_set_text(val_lbl, value);
    lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(val_lbl, lv_color_black(), 0);
    lv_obj_align(val_lbl, LV_ALIGN_CENTER, 0, -16);

    lv_obj_t *title_lbl = lv_label_create(card);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0x666666), 0);
    lv_obj_align(title_lbl, LV_ALIGN_CENTER, 0, 28);

    return card;
}

/* ── WiFi setup / provisioning screen ─────────────────────────────────── */

static lv_obj_t *s_setup_scr = NULL;   /* so we can free it after the dashboard */

static lv_obj_t *create_setup_screen(lv_obj_t **status_out)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "WiFi Setup");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 60, 40);

    /* QR code → the setup page URL, so the phone can open it without typing */
    const char *setup_url = "http://192.168.4.1";
    lv_obj_t *qr = lv_qrcode_create(scr);
    lv_qrcode_set_size(qr, 400);
    lv_qrcode_set_quiet_zone(qr, true);   /* white border helps phone cameras */
    lv_qrcode_update(qr, setup_url, strlen(setup_url));
    lv_obj_align(qr, LV_ALIGN_TOP_RIGHT, -60, 40);

    lv_obj_t *qr_cap = lv_label_create(scr);
    lv_label_set_text(qr_cap, "Scan to open the setup page");
    lv_obj_set_width(qr_cap, 400);
    lv_label_set_long_mode(qr_cap, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(qr_cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(qr_cap, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(qr_cap, lv_color_black(), 0);
    lv_obj_align(qr_cap, LV_ALIGN_TOP_RIGHT, -60, 455);

    char buf[448];
    snprintf(buf, sizeof(buf),
        "1. On your phone or laptop, join this WiFi network:\n"
        "       name:     %s\n"
        "       password: %s\n"
        "\n"
        "2. Then scan the QR code on the right\n"
        "    (or open %s in a browser).\n"
        "\n"
        "3. Pick your home network and enter its password.\n"
        "    The device will save it and connect automatically.",
        wifi_mgr_portal_ssid(), wifi_mgr_portal_pass(), setup_url);

    lv_obj_t *info = lv_label_create(scr);
    lv_label_set_text(info, buf);
    lv_obj_set_width(info, BSP_LCD_WIDTH - 600);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(info, lv_color_black(), 0);
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, 60, 170);

    lv_obj_t *status = lv_label_create(scr);
    lv_label_set_text(status, "Waiting for you to connect ...");
    lv_obj_set_width(status, BSP_LCD_WIDTH - 120);
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(status, lv_color_hex(0x555555), 0);
    lv_obj_align(status, LV_ALIGN_BOTTOM_LEFT, 60, -120);
    if (status_out) *status_out = status;

    return scr;
}

/* Show the portal, wait until the user configures WiFi or we time out.
 * Returns true if we end up connected.  Keeps the device awake throughout. */
static bool run_provisioning_portal(void)
{
    wifi_mgr_portal_start();

    lv_obj_t *status_lbl = NULL;
    s_setup_scr = create_setup_screen(&status_lbl);
    lv_screen_load(s_setup_scr);
    render_now();

    char ssid[WIFI_MGR_MAX_SSID];
    char pass[WIFI_MGR_MAX_PASS];
    bool connected = false;
    int64_t t0 = esp_timer_get_time();

    while (!connected) {
        int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
        if (elapsed_ms > PORTAL_TIMEOUT_MS) {
            set_status(status_lbl, "Timed out. WiFi skipped — will retry on next boot.");
            render_now();
            vTaskDelay(pdMS_TO_TICKS(1500));
            break;
        }

        if (wifi_mgr_portal_got_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
            set_status(status_lbl, "Connecting to \"%s\" ...", ssid);
            render_now();

            connected = wifi_mgr_connect_to(ssid, pass, WIFI_CONNECT_MS);
            if (connected) {
                set_status(status_lbl, "Connected! Loading dashboard ...");
                render_now();
                vTaskDelay(pdMS_TO_TICKS(800));
            } else {
                set_status(status_lbl,
                    "Couldn't connect to \"%s\". Check the password and try again.", ssid);
                render_now();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return connected;
}

/* ── Dashboard screen ─────────────────────────────────────────────────── */

typedef struct {
    int   offset_min;      /* UTC offset in minutes            */
    float lat, lon;        /* position of our public IP        */
    char  city[32];
    bool  has_pos;
} geo_info_t;

typedef struct {
    bool  ok;
    float temp_c;
    int   code;            /* WMO weather code */
    float wind_kmh;
} weather_t;

static geo_info_t s_geo;                 /* last lookup result (or NVS cache) */
static const char *wmo_text(int code);   /* defined further down */

static lv_obj_t *create_dashboard_screen(int pct, float temp_c, float humidity,
                                         const bsp_rtc_time_t *rtc, bool rtc_ok,
                                         uint32_t bat_mv, bool wifi_on,
                                         const char *wifi_ip, const weather_t *w)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Header */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "reTerminal E1003");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 50, 20);

    /* WiFi status under the title */
    lv_obj_t *wifi_lbl = lv_label_create(scr);
    if (wifi_on) set_status(wifi_lbl, "WiFi: connected  (%s)", wifi_ip);
    else         set_status(wifi_lbl, "WiFi: not connected");
    lv_obj_set_style_text_font(wifi_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(wifi_lbl, lv_color_hex(0x666666), 0);
    lv_obj_align(wifi_lbl, LV_ALIGN_TOP_LEFT, 50, 64);

    create_battery_bar(scr, pct, BSP_LCD_WIDTH - 310, 15, NULL);

    /* Separator */
    lv_obj_t *sep = lv_obj_create(scr);
    lv_obj_set_pos(sep, 50, 105);
    lv_obj_set_size(sep, BSP_LCD_WIDTH - 100, 2);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);

    /* Sensor cards (3 across) */
    int card_w = 520, card_h = 140, gap = 55;
    int total_w = 3 * card_w + 2 * gap;
    int card_x0 = (BSP_LCD_WIDTH - total_w) / 2;
    int card_y = 150;
    char buf[128];

    snprintf(buf, sizeof(buf), "%.1f °C", temp_c);
    create_card(scr, "Temperature", buf, card_x0, card_y, card_w, card_h);

    snprintf(buf, sizeof(buf), "%.0f %% RH", humidity);
    create_card(scr, "Humidity", buf, card_x0 + card_w + gap, card_y, card_w, card_h);

    if (rtc_ok) snprintf(buf, sizeof(buf), "%02d:%02d", rtc->hour, rtc->minute);
    else        snprintf(buf, sizeof(buf), "--:--");
    create_card(scr, "Time", buf, card_x0 + 2 * (card_w + gap), card_y, card_w, card_h);

    /* Second row: two wider cards */
    int wide_w = (2 * card_w + gap);
    int row2_y = card_y + card_h + 40;

    if (rtc_ok) {
        const char *months[] = {"","Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};
        const char *days[]  = {"Sunday","Monday","Tuesday","Wednesday",
                               "Thursday","Friday","Saturday"};
        int dow = day_of_week(rtc->day, rtc->month, rtc->year);
        snprintf(buf, sizeof(buf), "%s, %s %d, %d",
                 days[dow], months[rtc->month], rtc->day, rtc->year);
    } else {
        snprintf(buf, sizeof(buf), "RTC not set");
    }
    create_card(scr, "Date", buf, card_x0, row2_y, wide_w, card_h);

    snprintf(buf, sizeof(buf), "%lu mV", (unsigned long)bat_mv);
    create_card(scr, "Battery Voltage", buf,
                card_x0 + wide_w + gap, row2_y, card_w, card_h);

    /* Third row: outdoor weather (only when we managed to fetch it) */
    if (w && w->ok) {
        int row3_y = row2_y + card_h + 40;

        snprintf(buf, sizeof(buf), "%.1f °C", w->temp_c);
        create_card(scr, "Outdoor Temp", buf, card_x0, row3_y, card_w, card_h);

        create_card(scr, "Conditions", wmo_text(w->code),
                    card_x0 + card_w + gap, row3_y, card_w, card_h);

        snprintf(buf, sizeof(buf), "%.0f km/h", w->wind_kmh);
        create_card(scr, "Wind", buf,
                    card_x0 + 2 * (card_w + gap), row3_y, card_w, card_h);

        char cap[64];
        if (s_geo.city[0])
            snprintf(cap, sizeof(cap), "%s — open-meteo.com", s_geo.city);
        else
            snprintf(cap, sizeof(cap), "open-meteo.com");
        lv_obj_t *cap_lbl = lv_label_create(scr);
        lv_label_set_text(cap_lbl, cap);
        lv_obj_set_style_text_font(cap_lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(cap_lbl, lv_color_hex(0x888888), 0);
        lv_obj_align(cap_lbl, LV_ALIGN_TOP_MID, 0, row3_y + card_h + 12);
    }

    /* RTC voltage-low warning */
    if (rtc_ok && !rtc->voltage_ok) {
        lv_obj_t *warn = lv_label_create(scr);
        lv_label_set_text(warn, "RTC battery low — time may be unreliable");
        lv_obj_set_style_text_font(warn, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(warn, lv_color_hex(0x888888), 0);
        lv_obj_align(warn, LV_ALIGN_BOTTOM_MID, 0, -20);
    }

    return scr;
}

/* ── Internet time sync ─────────────────────────────────────────────── */

#define SNTP_SYNC_TIMEOUT_MS  20000
#define TIME_NVS_NAMESPACE    "time"
#define TIME_NVS_KEY_OFFSET   "utc_off"   /* last known UTC offset, minutes */
#define TIME_NVS_KEY_LAT      "lat_i"     /* last known latitude  × 10000 */
#define TIME_NVS_KEY_LON      "lon_i"     /* last known longitude × 10000 */
#define TIME_NVS_KEY_CITY     "city"


/* Small HTTP(S) GET into a buffer.  Auto-redirect is disabled so a captive
 * portal that hijacks the request fails fast instead of dragging us into a
 * TLS handshake. */
static bool http_get(const char *url, char *resp, size_t resp_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 6000,
        .disable_auto_redirect = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) return false;

    bool ok = false;
    if (esp_http_client_open(cl, 0) == ESP_OK) {
        esp_http_client_fetch_headers(cl);
        if (esp_http_client_get_status_code(cl) == 200) {
            int total = 0;
            while (total < (int)resp_len - 1) {
                int r = esp_http_client_read(cl, resp + total,
                                             resp_len - 1 - total);
                if (r <= 0) break;
                total += r;
            }
            resp[total] = '\0';
            ok = (total > 0);
        }
    }
    esp_http_client_close(cl);
    esp_http_client_cleanup(cl);
    return ok;
}

/* Tiny JSON helpers for the flat, well-known responses we fetch. */
static bool json_float(const char *json, const char *key, float *out)
{
    const char *p = strstr(json, key);
    if (!p) return false;
    return sscanf(p + strlen(key), "%f", out) == 1;
}

static bool json_int(const char *json, const char *key, int *out)
{
    const char *p = strstr(json, key);
    if (!p) return false;
    return sscanf(p + strlen(key), "%d", out) == 1;
}

static bool json_str(const char *json, const char *key, char *out, size_t out_len)
{
    const char *p = strstr(json, key);
    if (!p) return false;
    p = strchr(p + strlen(key), '"');        /* opening quote of the value */
    if (!p) return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1) out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}

/* Geo-IP lookup: UTC offset (daylight saving included) + position + city
 * of our public IP — used for the timezone and for the weather.  Several
 * sources are tried in order; reachability varies by country. */
static bool geo_lookup(geo_info_t *info)
{
    static char resp[2048];
    int sec = 0;

    /* ip-api.com — plain HTTP, offset + position in one request */
    if (http_get("http://ip-api.com/json/?fields=status,offset,lat,lon,city",
                 resp, sizeof(resp)) &&
        json_int(resp, "\"offset\":", &sec)) {
        info->offset_min = sec / 60;
        float lat = 0, lon = 0;
        if (json_float(resp, "\"lat\":", &lat) &&
            json_float(resp, "\"lon\":", &lon) &&
            (lat != 0 || lon != 0)) {
            info->lat = lat;
            info->lon = lon;
            info->has_pos = true;
            json_str(resp, "\"city\":", info->city, sizeof(info->city));
        }
        return true;
    }

    /* ipwho.is — HTTPS fallback (Let's Encrypt) */
    sec = 0;
    if (http_get("https://ipwho.is/", resp, sizeof(resp))) {
        const char *tz = strstr(resp, "\"timezone\"");
        if (tz && json_int(tz, "\"offset\":", &sec)) {
            info->offset_min = sec / 60;
            float lat = 0, lon = 0;
            if (json_float(resp, "\"latitude\":", &lat) &&
                json_float(resp, "\"longitude\":", &lon) &&
                (lat != 0 || lon != 0)) {
                info->lat = lat;
                info->lon = lon;
                info->has_pos = true;
                json_str(resp, "\"city\":", info->city, sizeof(info->city));
            }
            return true;
        }
    }
    return false;
}

static void cache_geo(const geo_info_t *info)
{
    nvs_handle_t h;
    if (nvs_open(TIME_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, TIME_NVS_KEY_OFFSET, info->offset_min);
    if (info->has_pos) {
        nvs_set_i32(h, TIME_NVS_KEY_LAT, (int32_t)(info->lat * 10000.0f));
        nvs_set_i32(h, TIME_NVS_KEY_LON, (int32_t)(info->lon * 10000.0f));
        if (info->city[0]) nvs_set_str(h, TIME_NVS_KEY_CITY, info->city);
    }
    nvs_commit(h);
    nvs_close(h);
}

/* Load the cached location info.  The offset is required, the position is
 * optional (older firmware only cached the offset). */
static bool cached_geo(geo_info_t *info)
{
    nvs_handle_t h;
    if (nvs_open(TIME_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    int32_t off = 0;
    esp_err_t e = nvs_get_i32(h, TIME_NVS_KEY_OFFSET, &off);
    if (e == ESP_OK) {
        info->offset_min = off;
        int32_t lat = 0, lon = 0;
        if (nvs_get_i32(h, TIME_NVS_KEY_LAT, &lat) == ESP_OK &&
            nvs_get_i32(h, TIME_NVS_KEY_LON, &lon) == ESP_OK &&
            (lat != 0 || lon != 0)) {
            info->lat = lat / 10000.0f;
            info->lon = lon / 10000.0f;
            info->has_pos = true;
            size_t cl = sizeof(info->city);
            nvs_get_str(h, TIME_NVS_KEY_CITY, info->city, &cl);
        }
    }
    nvs_close(h);
    return e == ESP_OK;
}

/* POSIX TZ string from a UTC offset in minutes.  Note the sign flip:
 * POSIX wants the value that, added to local time, gives UTC. */
static void make_tz_string(char *tz, size_t len, int offset_min)
{
    int sign = (offset_min < 0) ? -1 : 1;
    int abs_min = offset_min * sign;
    int hh = abs_min / 60, mm = abs_min % 60;
    if (mm) snprintf(tz, len, "<%s%02d%02d>%s%d:%02d",
                     sign > 0 ? "+" : "-", hh, mm,
                     sign > 0 ? "-" : "", hh, mm);
    else    snprintf(tz, len, "<%s%02d>%s%d",
                     sign > 0 ? "+" : "-", hh,
                     sign > 0 ? "-" : "", hh);
}

/* Get UTC time via SNTP.  True if the time is valid afterwards.
 * Uses Chinese NTP mirrors — pool.ntp.org is often unreachable there. */
static bool sntp_sync(uint32_t timeout_ms)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "ntp.tencent.com");
    esp_sntp_setservername(2, "cn.pool.ntp.org");
    esp_sntp_init();

    int64_t t0 = esp_timer_get_time();
    for (;;) {
        sntp_sync_status_t st = sntp_get_sync_status();
        if (st == SNTP_SYNC_STATUS_COMPLETED) {
            esp_sntp_stop();
            return true;
        }

        if ((esp_timer_get_time() - t0) / 1000 >= timeout_ms) {
            ESP_LOGW(TAG, "SNTP timed out (status %d)", (int)st);
            esp_sntp_stop();
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));   /* lwIP retries internally */
    }
}

/* Sync the clock from the internet: UTC from SNTP + local zone from a
 * geo-IP lookup of our public IP (falls back to the last known offset,
 * then to UTC).  Writes the result into the PCF8563 RTC. */
static bool sync_time_from_internet(void)
{
    memset(&s_geo, 0, sizeof(s_geo));
    if (geo_lookup(&s_geo)) {
        cache_geo(&s_geo);
    } else if (cached_geo(&s_geo)) {
        ESP_LOGW(TAG, "Geo-IP lookup failed — using last known offset/location");
    } else {
        /* No lookup result and nothing cached — writing an arbitrary local
         * time would be worse than keeping the (stale but honest) RTC. */
        ESP_LOGW(TAG, "Timezone unknown — RTC left unchanged");
        return false;
    }

    int offset_min = s_geo.offset_min;
    char tz[24];
    make_tz_string(tz, sizeof(tz), offset_min);
    setenv("TZ", tz, 1);
    tzset();

    if (!sntp_sync(SNTP_SYNC_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "SNTP sync failed — RTC not updated");
        return false;
    }

    time_t now;
    struct tm t;
    time(&now);
    localtime_r(&now, &t);

    bsp_rtc_time_t rt = {
        .year = t.tm_year + 1900, .month = t.tm_mon + 1, .day = t.tm_mday,
        .hour = t.tm_hour, .minute = t.tm_min, .second = t.tm_sec,
    };
    if (bsp_rtc_set_time(&rt) != ESP_OK) {
        ESP_LOGW(TAG, "bsp_rtc_set_time failed");
        return false;
    }

    ESP_LOGI(TAG, "RTC set from NTP: %04d-%02d-%02d %02d:%02d:%02d (UTC%+03d:%02d)",
             rt.year, rt.month, rt.day, rt.hour, rt.minute, rt.second,
             offset_min / 60,
             offset_min < 0 ? -(offset_min % 60) : offset_min % 60);
    return true;
}

/* ── Weather ────────────────────────────────────────────────────────── */

/* WMO weather-code → short description (open-meteo's coding). */
static const char *wmo_text(int code)
{
    switch (code) {
    case 0:  return "Clear sky";
    case 1:  return "Mainly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: case 56: case 57: return "Drizzle";
    case 61: case 63: case 65: case 66: case 67: return "Rain";
    case 71: case 73: case 75: case 77: return "Snow";
    case 80: case 81: case 82: return "Rain showers";
    case 85: case 86: return "Snow showers";
    case 95: case 96: case 99: return "Thunderstorm";
    default: return "—";
    }
}

/* Current conditions from open-meteo.com (free, no API key) for the
 * position in s_geo. */
static bool fetch_weather(weather_t *w)
{
    memset(w, 0, sizeof(*w));
    if (!s_geo.has_pos) return false;

    char url[220];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,weather_code,wind_speed_10m"
        "&timezone=auto",
        s_geo.lat, s_geo.lon);

    static char resp[2048];
    if (!http_get(url, resp, sizeof(resp))) {
        ESP_LOGW(TAG, "Weather fetch failed");
        return false;
    }

    /* The values we want live inside the "current":{...} object. */
    const char *cur = strstr(resp, "\"current\":{");
    if (!cur) return false;

    float t = 0, wind = 0;
    int code = 0;
    if (!json_float(cur, "\"temperature_2m\":", &t)) return false;
    json_int(cur, "\"weather_code\":", &code);
    json_float(cur, "\"wind_speed_10m\":", &wind);

    w->ok      = true;
    w->temp_c  = t;
    w->code    = code;
    w->wind_kmh = wind;
    ESP_LOGI(TAG, "Weather (%s): %.1f C, %s, wind %.0f km/h",
             s_geo.city[0] ? s_geo.city : "unknown",
             t, wmo_text(code), wind);
    return true;
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

    /* ── WiFi: connect to a saved network, or fall back to the portal ── */
    wifi_mgr_init();
    bool wifi_on = false;
    if (wifi_mgr_has_credentials()) {
        wifi_on = wifi_mgr_connect_saved(WIFI_CONNECT_MS);
        if (!wifi_on) ESP_LOGW(TAG, "Saved network unreachable — falling back to portal");
    }
    if (!wifi_on) {
        wifi_on = run_provisioning_portal();
    }

    char wifi_ip[16] = {0};
    if (wifi_on) wifi_mgr_get_ip_str(wifi_ip, sizeof(wifi_ip));

    /* ── Time: sync from the internet and update the RTC ── */
    if (wifi_on && sync_time_from_internet()) {
        rtc_ok = (bsp_rtc_read_time(&rtc) == ESP_OK);
        if (rtc_ok && (rtc.year < 2024 || rtc.year > 2099)) rtc_ok = false;
    }

    /* ── Outdoor weather for the dashboard ── */
    weather_t weather = {0};
    if (wifi_on) fetch_weather(&weather);

    /* ── Dashboard ── */
    int pct = battery_pct(bat_mv);
    lv_obj_t *dash = create_dashboard_screen(pct, temp_c, humidity, &rtc, rtc_ok,
                                             bat_mv, wifi_on, wifi_ip, &weather);
    lv_screen_load(dash);
    if (s_setup_scr) { lv_obj_del(s_setup_scr); s_setup_scr = NULL; }

    ESP_LOGI(TAG, "Rendering...");
    render_now();
    ESP_LOGI(TAG, "Done — %d battery%%, WiFi %s", pct, wifi_on ? "on" : "off");

    /* ── Power down WiFi, then sleep ── */
    wifi_mgr_stop();
    vTaskDelay(pdMS_TO_TICKS(30000));
    bsp_deep_sleep_enter(30, true);
}
