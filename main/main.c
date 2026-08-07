#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "bsp.h"

static const char *TAG = "app";

/* --- Hardcoded WiFi credentials ------------------------------------------- */
#define WIFI_SSID     "your-ssid"
#define WIFI_PASSWORD "your-password"

static EventGroupHandle_t s_wifi_evt;
#define WIFI_CONNECTED_BIT BIT0

/* ==========================================================================
 * WiFi
 * ========================================================================== */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected — reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        xEventGroupSetBits(s_wifi_evt, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_connect(void)
{
    s_wifi_evt = xEventGroupCreate();

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi connecting to %s ...", WIFI_SSID);

    /* Wait up to 30 s for connection */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_evt, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(30000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected.");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "WiFi connect timed out.");
    return ESP_ERR_TIMEOUT;
}

/* ==========================================================================
 * App
 * ========================================================================== */

void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "reTerminal E1003 — ESP-IDF v6");
    ESP_LOGI(TAG, "Target: %s | cores: %d | rev: %d",
             CONFIG_IDF_TARGET,
             chip_info.cores,
             chip_info.revision);

    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "Flash: %lu MB (%s)",
                 (unsigned long)(flash_size >> 20),
                 (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
    }

    size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "PSRAM: %s (%u MB)",
             psram_size > 0 ? "present" : "not detected",
             (unsigned)(psram_size >> 20));

    /* Initialise the board */
    esp_err_t err = bsp_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BSP init returned error: 0x%x", err);
    }

    /* --- Probe display & touch --- */
    bsp_display_probe();
    bsp_touch_probe();

    /* Read battery */
    uint32_t bat_mv = 0;
    if (bsp_battery_read_mv(&bat_mv) == ESP_OK) {
        ESP_LOGI(TAG, "Battery: %lu mV (%.2f V)",
                 (unsigned long)bat_mv, bat_mv / 1000.0f);
    }

    /* Blink the LED a few times */
    for (int i = 0; i < 3; i++) {
        BSP_LED_ON();
        vTaskDelay(pdMS_TO_TICKS(200));
        BSP_LED_OFF();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* --- RTC: set time if voltage was low (battery drained) --- */
    bsp_rtc_time_t rtc;
    if (bsp_rtc_read_time(&rtc) == ESP_OK && !rtc.voltage_ok) {
        ESP_LOGW(TAG, "RTC battery was drained — setting time from compile timestamp");

        /* Parse __DATE__ "Aug  8 2026" and __TIME__ "01:23:45" */
        const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
        bsp_rtc_time_t now = {0};
        for (int i = 0; i < 12; i++) {
            if (strncmp(__DATE__, months + i * 3, 3) == 0) { now.month = i + 1; break; }
        }
        now.year   = atoi(__DATE__ + 7);
        now.day    = atoi(__DATE__ + 4);
        now.hour   = atoi(__TIME__);
        now.minute = atoi(__TIME__ + 3);
        now.second = atoi(__TIME__ + 6);

        bsp_rtc_set_time(&now);
        ESP_LOGI(TAG, "RTC set to: %04d-%02d-%02d %02d:%02d:%02d",
                 now.year, now.month, now.day, now.hour, now.minute, now.second);
    }

    /* --- WiFi --- */
    wifi_connect();

    ESP_LOGI(TAG, "Board ready — polling sensors every 5 s.");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        /* --- RTC --- */
        if (bsp_rtc_read_time(&rtc) == ESP_OK) {
            ESP_LOGI(TAG, "RTC: %04d-%02d-%02d %02d:%02d:%02d%s",
                     rtc.year, rtc.month, rtc.day,
                     rtc.hour, rtc.minute, rtc.second,
                     rtc.voltage_ok ? "" : " [VL]");
        } else {
            ESP_LOGW(TAG, "RTC: read failed");
        }

        /* --- SHT4x --- */
        float temp_c, humidity;
        if (bsp_sht4x_read(&temp_c, &humidity) == ESP_OK) {
            ESP_LOGI(TAG, "SHT4x: %.1f C  %.1f %%RH", temp_c, humidity);
        } else {
            ESP_LOGW(TAG, "SHT4x: read failed");
        }

        /* --- Battery --- */
        uint32_t bat_mv;
        if (bsp_battery_read_mv(&bat_mv) == ESP_OK) {
            ESP_LOGI(TAG, "Battery: %lu mV (%.2f V)",
                     (unsigned long)bat_mv, bat_mv / 1000.0f);
        }
    }
}
