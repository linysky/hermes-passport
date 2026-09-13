// main/hermes_wifi_prov.h — WiFi provisioning for hermes-bridge
// Uses WiFi STA mode with NVS-stored credentials
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// Callback when WiFi connection is established
typedef void (*wifi_prov_connected_cb_t)(const char *ip_addr);
typedef void (*wifi_prov_failed_cb_t)(esp_err_t err);

// Start WiFi provisioning
// Loads credentials from NVS and connects to WiFi
esp_err_t hermes_wifi_prov_start(wifi_prov_connected_cb_t on_connected,
                                  wifi_prov_failed_cb_t on_failed);

// Stop WiFi provisioning
void hermes_wifi_prov_stop(void);

// Check if WiFi is currently connected
bool hermes_wifi_prov_is_connected(void);

// Get current IP address (returns empty string if not connected)
const char *hermes_wifi_prov_get_ip(void);

// Get server IP address
const char *hermes_wifi_prov_get_server_ip(void);

// Get server port
uint16_t hermes_wifi_prov_get_server_port(void);

// Save WiFi config to NVS
esp_err_t hermes_wifi_prov_save_config(const char *ssid, const char *password,
                                        const char *server_ip, uint16_t server_port);
