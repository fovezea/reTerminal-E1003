/*
 * wifi_manager.c — WiFi provisioning & connection manager
 *
 * See wifi_manager.h for the intended flow.  Summary:
 *   - Up to WIFI_MGR_MAX_NETWORKS credentials live in NVS (namespace "wifi"),
 *     kept in most-recently-used order (index 0 = highest priority).
 *   - wifi_mgr_connect_saved() scans, then connects to whichever saved
 *     network is in range (highest priority first) — like a phone.
 *   - wifi_mgr_portal_start() runs a SoftAP + web portal for entering creds.
 *   - wifi_mgr_connect_to() connects with freshly submitted creds and, on
 *     success, stores them so the next boot connects automatically.
 *   - Legacy single-network entries ("ssid"/"pass") are migrated on load.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "lwip/ip4_addr.h"

#include "wifi_manager.h"

static const char *TAG = "wifi_mgr";

/* Provisioning SoftAP identity (shown on the e-paper setup screen). */
#define WIFI_MGR_AP_SSID   "reTerminal-E1003"
#define WIFI_MGR_AP_PASS   "reterminal"      /* WPA2-PSK needs >= 8 chars */

#define NVS_NAMESPACE      "wifi"
#define NVS_KEY_COUNT      "netcount"     /* u8: how many networks are saved */
#define NVS_KEY_NET_FMT    "net%d"        /* blob per network, 0 = priority */
/* Legacy (single-network) keys — migrated into the list on first load. */
#define NVS_KEY_SSID       "ssid"
#define NVS_KEY_PASS       "pass"

#define WIFI_MGR_MAX_NETWORKS 8
#define CONNECT_MAX_RETRY  3
#define SCAN_MAX_AP        12
#define PORTAL_MAX_CLIENTS 4

/* Event-group bits */
#define BIT_CONN   BIT0   /* station got an IP          */
#define BIT_FAIL   BIT1   /* retries exhausted          */
#define BIT_CREDS  BIT2   /* portal received credentials */

/* Embedded config page (EMBED_TXTFILES in CMakeLists) */
extern const char index_html_start[] asm("_binary_index_html_start");

/* ------------------------------------------------------------------ state */
typedef struct {
    char ssid[WIFI_MGR_MAX_SSID];
    char pass[WIFI_MGR_MAX_PASS];
} wifi_net_t;

static bool                 s_inited    = false;
static bool                 s_connected = false;
static int                  s_retry     = 0;
static EventGroupHandle_t   s_evt       = NULL;
static esp_netif_t         *s_netif_sta = NULL;
static esp_netif_t         *s_netif_ap  = NULL;
static httpd_handle_t       s_httpd     = NULL;

/* Saved networks, most-recently-used first (index 0 is tried first). */
static wifi_net_t s_networks[WIFI_MGR_MAX_NETWORKS];
static int        s_net_count = 0;

static char s_portal_ssid[WIFI_MGR_MAX_SSID];
static char s_portal_pass[WIFI_MGR_MAX_PASS];

/* Cached AP scan, taken once when the portal starts (before any client is
 * connected).  Scanning later would channel-hop the STA and disrupt the
 * client that is currently browsing the portal. */
static wifi_ap_record_t s_scan_cache[SCAN_MAX_AP];
static int              s_scan_count = 0;

/* ===================================================================== NVS */

/* Load the network list.  Falls back to the legacy single "ssid"/"pass"
 * keys so credentials saved by older firmware carry over. */
static void nvs_load_networks(void)
{
    s_net_count = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t count = 0;
    if (nvs_get_u8(h, NVS_KEY_COUNT, &count) == ESP_OK && count > 0) {
        if (count > WIFI_MGR_MAX_NETWORKS) count = WIFI_MGR_MAX_NETWORKS;
        char key[16];
        for (int i = 0; i < count; i++) {
            wifi_net_t net;
            size_t len = sizeof(net);
            snprintf(key, sizeof(key), NVS_KEY_NET_FMT, i);
            if (nvs_get_blob(h, key, &net, &len) != ESP_OK) continue;
            net.ssid[WIFI_MGR_MAX_SSID - 1] = '\0';
            net.pass[WIFI_MGR_MAX_PASS - 1] = '\0';
            if (net.ssid[0]) s_networks[s_net_count++] = net;
        }
    } else {
        /* Legacy single-network layout */
        char ssid[WIFI_MGR_MAX_SSID] = {0};
        char pass[WIFI_MGR_MAX_PASS] = {0};
        size_t sl = sizeof(ssid), pl = sizeof(pass);
        esp_err_t e = nvs_get_str(h, NVS_KEY_SSID, ssid, &sl);
        nvs_get_str(h, NVS_KEY_PASS, pass, &pl);   /* may legitimately fail */
        if (e == ESP_OK && ssid[0]) {
            strncpy(s_networks[0].ssid, ssid, WIFI_MGR_MAX_SSID - 1);
            strncpy(s_networks[0].pass, pass, WIFI_MGR_MAX_PASS - 1);
            s_net_count = 1;
        }
    }
    nvs_close(h);
}

static void nvs_save_networks(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(RW) failed");
        return;
    }
    nvs_set_u8(h, NVS_KEY_COUNT, (uint8_t)s_net_count);
    char key[16];
    for (int i = 0; i < s_net_count; i++) {
        snprintf(key, sizeof(key), NVS_KEY_NET_FMT, i);
        nvs_set_blob(h, key, &s_networks[i], sizeof(wifi_net_t));
    }
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Saved %d network(s) to NVS (%s)", s_net_count, esp_err_to_name(err));
}

/* Move network `index` to the front (most recently used). */
static void promote_network(int index)
{
    if (index <= 0 || index >= s_net_count) return;
    wifi_net_t tmp = s_networks[index];
    memmove(&s_networks[1], &s_networks[0], (size_t)index * sizeof(wifi_net_t));
    s_networks[0] = tmp;
}

/* Insert or update a network and make it the top-priority one. */
static void remember_network(const char *ssid, const char *pass)
{
    for (int i = 0; i < s_net_count; i++) {
        if (strcmp(s_networks[i].ssid, ssid) == 0) {
            strncpy(s_networks[i].pass, pass ? pass : "", WIFI_MGR_MAX_PASS - 1);
            s_networks[i].pass[WIFI_MGR_MAX_PASS - 1] = '\0';
            promote_network(i);
            return;
        }
    }
    if (s_net_count >= WIFI_MGR_MAX_NETWORKS) s_net_count--;  /* drop lowest priority */
    memmove(&s_networks[1], &s_networks[0], (size_t)s_net_count * sizeof(wifi_net_t));
    strncpy(s_networks[0].ssid, ssid, WIFI_MGR_MAX_SSID - 1);
    s_networks[0].ssid[WIFI_MGR_MAX_SSID - 1] = '\0';
    strncpy(s_networks[0].pass, pass ? pass : "", WIFI_MGR_MAX_PASS - 1);
    s_networks[0].pass[WIFI_MGR_MAX_PASS - 1] = '\0';
    s_net_count++;
}

static bool ssid_in_scan(const char *ssid)
{
    for (int i = 0; i < s_scan_count; i++)
        if (strcmp((const char *)s_scan_cache[i].ssid, ssid) == 0) return true;
    return false;
}

/* ============================================================ event handler */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry < CONNECT_MAX_RETRY) {
            s_retry++;
            ESP_LOGI(TAG, "STA disconnected, retry %d/%d", s_retry, CONNECT_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "STA disconnected, retries exhausted");
            xEventGroupSetBits(s_evt, BIT_FAIL);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected — IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry = 0;
        s_connected = true;
        xEventGroupSetBits(s_evt, BIT_CONN);
    }
}

/* ======================================================== STA connect core */
static bool sta_connect_internal(const char *ssid, const char *pass,
                                 uint32_t timeout_ms)
{
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK
                                                   : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);

    xEventGroupClearBits(s_evt, BIT_CONN | BIT_FAIL);
    s_retry = 0;
    esp_wifi_set_ps(WIFI_PS_NONE);   /* full throughput while connecting */
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_evt, BIT_CONN | BIT_FAIL,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (bits & BIT_CONN) return true;

    esp_wifi_disconnect();   /* stop any pending retries */
    return false;
}

/* =============================================================== HTTP bits */
static void urldecode(char *dst, const char *src, size_t dst_len)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di < dst_len - 1; si++) {
        if (src[si] == '+') {
            dst[di++] = ' ';
        } else if (src[si] == '%' && isxdigit((uint8_t)src[si + 1]) &&
                   isxdigit((uint8_t)src[si + 2])) {
            char hex[3] = { src[si + 1], src[si + 2], 0 };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

static void form_get(const char *body, const char *key, char *out, size_t out_len)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(body, pat);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(pat);
    const char *e = strchr(p, '&');
    size_t len = e ? (size_t)(e - p) : strlen(p);

    char tmp[WIFI_MGR_MAX_PASS];
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, p, len);
    tmp[len] = '\0';
    urldecode(out, tmp, out_len);
}

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, index_html_start);
    return ESP_OK;
}

static esp_err_t redirect_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_sendstr(req, "Redirecting to WiFi setup");
    return ESP_OK;
}

/* Append a JSON-safe copy of src (strip characters that would break JSON). */
static void json_escape(char *dst, const char *src, size_t dst_len)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di < dst_len - 1; si++) {
        char c = src[si];
        if (c == '"' || c == '\\' || c < 0x20) c = '?';
        dst[di++] = c;
    }
    dst[di] = '\0';
}

/* Run a scan and cache the results.  Called once from wifi_mgr_portal_start()
 * while no client is connected, so the STA channel-hop doesn't drop anyone. */
static void do_scan_cache(void)
{
    wifi_scan_config_t scfg = { .show_hidden = false };
    if (esp_wifi_scan_start(&scfg, true) != ESP_OK) {
        ESP_LOGW(TAG, "scan_start failed");
        s_scan_count = 0;
        return;
    }
    uint16_t num = SCAN_MAX_AP;
    memset(s_scan_cache, 0, sizeof(s_scan_cache));
    esp_wifi_scan_get_ap_records(&num, s_scan_cache);
    s_scan_count = num;
    ESP_LOGI(TAG, "Scan cached %d APs", s_scan_count);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    size_t cap = 2048;
    char *json = malloc(cap);
    if (!json) { httpd_resp_send_500(req); return ESP_FAIL; }

    size_t off = 0;
    off += snprintf(json + off, cap - off, "[");
    bool first = true;
    for (int i = 0; i < s_scan_count && i < SCAN_MAX_AP; i++) {
        char safe[40];
        json_escape(safe, (const char *)s_scan_cache[i].ssid, sizeof(safe));
        if (!safe[0]) continue;
        off += snprintf(json + off, cap - off,
                        "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%d}",
                        first ? "" : ",",
                        safe, s_scan_cache[i].rssi,
                        s_scan_cache[i].authmode == WIFI_AUTH_OPEN ? 1 : 0);
        first = false;
        if (off >= cap - 64) break;
    }
    off += snprintf(json + off, cap - off, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t save_post(httpd_req_t *req)
{
    static char body[300];
    int total = 0, ret;
    while (total < (int)sizeof(body) - 1) {
        ret = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (ret <= 0) break;
        total += ret;
    }
    body[total] = '\0';

    char ssid[WIFI_MGR_MAX_SSID] = {0};
    char pass[WIFI_MGR_MAX_PASS] = {0};
    form_get(body, "ssid", ssid, sizeof(ssid));
    form_get(body, "pass", pass, sizeof(pass));

    if (ssid[0]) {
        strncpy(s_portal_ssid, ssid, sizeof(s_portal_ssid) - 1);
        strncpy(s_portal_pass, pass, sizeof(s_portal_pass) - 1);
        xEventGroupSetBits(s_evt, BIT_CREDS);
        ESP_LOGI(TAG, "Portal received SSID: %s", ssid);
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req,
            "<!DOCTYPE html><html><body style='font-family:sans-serif;padding:24px'>"
            "<h3>Saved!</h3><p>The device is now connecting to your network. "
            "You can close this page.</p></body></html>");
    } else {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req,
            "<!DOCTYPE html><html><body style='font-family:sans-serif;padding:24px'>"
            "<h3>No SSID provided.</h3><p><a href='/'>Go back</a></p></body></html>");
    }
    return ESP_OK;
}

static esp_err_t start_httpd(void)
{
    if (s_httpd) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 16;
    config.lru_purge_enable = true;

    if (httpd_start(&s_httpd, &config) != ESP_OK) {
        s_httpd = NULL;
        ESP_LOGE(TAG, "httpd_start failed");
        return ESP_FAIL;
    }

    static const httpd_uri_t root = { .uri = "/",        .method = HTTP_GET,  .handler = root_get };
    static const httpd_uri_t scan = { .uri = "/scan",    .method = HTTP_GET,  .handler = scan_get };
    static const httpd_uri_t save = { .uri = "/save",    .method = HTTP_POST, .handler = save_post };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &save);

    /* Captive-portal detection endpoints → redirect to our page. */
    static const char *cp_uris[] = {
        "/generate_204", "/gen_204", "/hotspot-detect.html",
        "/library/test/success.html", "/connecttest.txt", "/redirect",
        "/fwlink", "/ncsi.txt", "/canonical.html", NULL
    };
    for (int i = 0; cp_uris[i]; i++) {
        httpd_uri_t r = { .uri = cp_uris[i], .method = HTTP_GET, .handler = redirect_get };
        httpd_register_uri_handler(s_httpd, &r);
    }

    ESP_LOGI(TAG, "Config portal at http://192.168.4.1/");
    return ESP_OK;
}

/* ================================================================= public */
esp_err_t wifi_mgr_init(void)
{
    if (s_inited) return ESP_OK;

    /* NVS (erase + reinit if the layout is stale) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NEW_VERSION_FOUND || ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_LOGW(TAG, "NVS reset (%s)", esp_err_to_name(ret));
        nvs_flash_erase();
        nvs_flash_init();
    }

    nvs_load_networks();
    if (s_net_count > 0)
        ESP_LOGI(TAG, "%d saved network(s), priority: \"%s\"",
                 s_net_count, s_networks[0].ssid);
    else
        ESP_LOGI(TAG, "No saved WiFi credentials");

    if (!s_evt) s_evt = xEventGroupCreate();

    esp_netif_init();
    esp_event_loop_create_default();
    s_netif_sta = esp_netif_create_default_wifi_sta();
    s_netif_ap  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, NULL);

    s_inited = true;
    ESP_LOGI(TAG, "wifi_mgr ready");
    return ESP_OK;
}

bool wifi_mgr_has_credentials(void) { return s_net_count > 0; }

bool wifi_mgr_connect_saved(uint32_t timeout_ms)
{
    if (!s_inited || s_net_count == 0) return false;

    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    /* Find out which saved networks are actually in range. */
    vTaskDelay(pdMS_TO_TICKS(200));
    do_scan_cache();

    for (int i = 0; i < s_net_count; i++) {
        /* Skip networks the scan didn't see.  If the scan itself failed
         * (s_scan_count == 0), try every saved network in priority order. */
        if (s_scan_count > 0 && !ssid_in_scan(s_networks[i].ssid)) {
            ESP_LOGI(TAG, "Saved network \"%s\" not in range", s_networks[i].ssid);
            continue;
        }
        ESP_LOGI(TAG, "Connecting to saved network \"%s\" ...", s_networks[i].ssid);
        if (sta_connect_internal(s_networks[i].ssid, s_networks[i].pass, timeout_ms)) {
            if (i > 0) {                 /* promote the winner for next boot */
                promote_network(i);
                nvs_save_networks();
            }
            return true;
        }
    }
    return false;
}

bool wifi_mgr_connect_to(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    if (!s_inited || !ssid || !ssid[0]) return false;

    /* Make sure the STA side is up (portal may already hold APSTA mode). */
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA) {
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
    }

    ESP_LOGI(TAG, "Connecting to \"%s\" ...", ssid);
    bool ok = sta_connect_internal(ssid, pass ? pass : "", timeout_ms);
    if (ok) {
        remember_network(ssid, pass ? pass : "");
        nvs_save_networks();
    }
    return ok;
}

esp_err_t wifi_mgr_portal_start(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_APSTA);   /* AP for the user, STA to test with */

    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, WIFI_MGR_AP_SSID, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(WIFI_MGR_AP_SSID);
    strncpy((char *)ap_cfg.ap.password, WIFI_MGR_AP_PASS, sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.max_connection = PORTAL_MAX_CLIENTS;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.channel = 1;
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_start();

    ESP_LOGI(TAG, "Portal AP up: SSID \"%s\" pass \"%s\"",
             WIFI_MGR_AP_SSID, WIFI_MGR_AP_PASS);

    /* Scan while no client is connected yet, so the page loads instantly and
     * the channel-hop doesn't disrupt anyone. */
    vTaskDelay(pdMS_TO_TICKS(200));
    do_scan_cache();

    return start_httpd();
}

bool wifi_mgr_portal_got_credentials(char *ssid, size_t ssid_len,
                                     char *pass, size_t pass_len)
{
    if (!s_evt) return false;
    if (!(xEventGroupGetBits(s_evt) & BIT_CREDS)) return false;
    xEventGroupClearBits(s_evt, BIT_CREDS);

    strncpy(ssid, s_portal_ssid, ssid_len - 1); ssid[ssid_len - 1] = '\0';
    strncpy(pass, s_portal_pass, pass_len - 1); pass[pass_len - 1] = '\0';
    return true;
}

bool wifi_mgr_is_connected(void) { return s_connected; }

esp_err_t wifi_mgr_get_ip_str(char *buf, size_t len)
{
    if (!s_connected || !s_netif_sta) return ESP_ERR_INVALID_STATE;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_netif_sta, &ip) != ESP_OK)
        return ESP_ERR_INVALID_STATE;
    snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    return ESP_OK;
}

const char *wifi_mgr_portal_ssid(void) { return WIFI_MGR_AP_SSID; }
const char *wifi_mgr_portal_pass(void) { return WIFI_MGR_AP_PASS; }

void wifi_mgr_stop(void)
{
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    if (s_inited) {
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    s_connected = false;
    ESP_LOGI(TAG, "WiFi stopped");
}
