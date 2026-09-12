// main/hermes_wifi_prov.c — WiFi provisioning via BLUFI for hermes-bridge
// Uses BLE to receive WiFi credentials from ESP Config mobile app
// Reference: demo/blufi-provisioning branch
#pragma once

#include "esp_err.h"
#include <stdbool.h>

// Callback when WiFi credentials are received and connection is established
typedef void (*wifi_prov_connected_cb_t)(const char *ip_addr);
typedef void (*wifi_prov_failed_cb_t)(esp_err_t err);

// Start BLUFI provisioning
// Returns ESP_OK if BLE advertising started
esp_err_t hermes_wifi_prov_start(wifi_prov_connected_cb_t on_connected,
                                  wifi_prov_failed_cb_t on_failed);

// Stop BLUFI provisioning
void hermes_wifi_prov_stop(void);

// Check if WiFi is currently connected
bool hermes_wifi_prov_is_connected(void);

// Get current IP address (returns empty string if not connected)
const char *hermes_wifi_prov_get_ip(void);
