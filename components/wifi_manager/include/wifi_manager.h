/*
 * wifi_manager.h — WiFi provisioning & connection manager
 *
 * Provides the "usual" provisioning flow for the reTerminal E1003:
 *   - Load a LIST of saved credentials from NVS (most recently used first),
 *     scan, and connect as a station (STA) to whichever saved network is
 *     in range — like a phone remembers several networks.
 *   - If there are none, or no saved network can be reached, run a
 *     SoftAP + web config portal so the user can enter SSID + password.
 *   - Newly submitted credentials are added to the list (never overwrite
 *     the others), so the device keeps remembering every network.
 *
 * The component is self-contained: it owns NVS access, the WiFi driver,
 * the SoftAP, and the HTTP server.  The application only orchestrates it
 * and renders status to the user.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum lengths for stored credentials (IEEE 802.11: SSID ≤ 32, WPA PSK ≤ 63). */
#define WIFI_MGR_MAX_SSID   33   /* 32 + NUL */
#define WIFI_MGR_MAX_PASS   65   /* 64 + NUL */

/**
 * @brief Initialise NVS, esp_netif, the default event loop and the WiFi driver.
 *
 * Idempotent — safe to call once at startup.  Loads any saved credentials so
 * wifi_mgr_has_credentials()/wifi_mgr_connect_saved() can be used immediately.
 *
 * @return ESP_OK on success.
 */
esp_err_t wifi_mgr_init(void);

/** @return true if a WiFi network (SSID) is saved in NVS. */
bool wifi_mgr_has_credentials(void);

/**
 * @brief Connect as a station to the best available saved network.
 *
 * Scans for available APs, then tries each saved network in priority order
 * (most recently used first), skipping ones that are out of range.  Each
 * attempt blocks up to timeout_ms, retrying internally.  Returns true once
 * the station has obtained an IP address.  Returns false if no saved
 * network can be reached — in which case the caller should fall back to
 * wifi_mgr_portal_start().
 *
 * @param timeout_ms  Time budget for each connection attempt.
 */
bool wifi_mgr_connect_saved(uint32_t timeout_ms);

/**
 * @brief Connect as a station to an explicit SSID/password.
 *
 * Used after the user submits credentials via the portal.  On success the
 * credentials are added to the saved network list (existing entries are
 * kept) and become the top priority, so the next boot connects to them
 * automatically while still remembering the other networks.
 *
 * @param ssid       Network name (NUL-terminated).
 * @param pass       Network password ("" for open networks).
 * @param timeout_ms Overall time budget for the connection attempt.
 * @return true once an IP address is obtained.
 */
bool wifi_mgr_connect_to(const char *ssid, const char *pass, uint32_t timeout_ms);

/**
 * @brief Start the SoftAP + web config portal.  Returns immediately.
 *
 * The portal stays up until wifi_mgr_stop() is called (typically after the
 * user submits credentials and connects, or after a timeout).
 *
 * @return ESP_OK on success.
 */
esp_err_t wifi_mgr_portal_start(void);

/**
 * @brief Poll whether the user has submitted credentials via the portal.
 *
 * If credentials were submitted since the last call, they are copied into
 * the provided buffers and true is returned (and the internal flag cleared).
 *
 * @param[out] ssid      Destination buffer, at least WIFI_MGR_MAX_SSID bytes.
 * @param      ssid_len  Size of the ssid buffer.
 * @param[out] pass      Destination buffer, at least WIFI_MGR_MAX_PASS bytes.
 * @param      pass_len  Size of the pass buffer.
 * @return true if new credentials were submitted.
 */
bool wifi_mgr_portal_got_credentials(char *ssid, size_t ssid_len,
                                     char *pass, size_t pass_len);

/** @return true if the station is currently connected with an IP address. */
bool wifi_mgr_is_connected(void);

/**
 * @brief Write the current station IP address as a dotted-quad string.
 *
 * @param[out] buf  Destination buffer.
 * @param      len  Buffer size (16 is enough, e.g. "255.255.255.255").
 * @return ESP_OK, or ESP_ERR_INVALID_STATE if not connected.
 */
esp_err_t wifi_mgr_get_ip_str(char *buf, size_t len);

/** @brief Human-readable SSID of the provisioning SoftAP (for display). */
const char *wifi_mgr_portal_ssid(void);

/** @brief Human-readable password of the provisioning SoftAP (for display). */
const char *wifi_mgr_portal_pass(void);

/**
 * @brief Stop the portal (if running) and the WiFi driver to save power.
 *
 * Call before entering deep sleep.  After this call the radio is off; call
 * wifi_mgr_init()/connect again on the next wake if needed.
 */
void wifi_mgr_stop(void);

#ifdef __cplusplus
}
#endif
