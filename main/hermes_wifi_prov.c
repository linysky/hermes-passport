// main/hermes_wifi_prov.c — WiFi provisioning for hermes-bridge
// Uses WiFi STA mode with NVS-stored credentials
// BLUFI support can be added later when wifi lib is fixed

#include "hermes_wifi_prov.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "wifi_prov";

// State
static esp_netif_t *s_sta_netif = NULL;
static esp_event_handler_instance_t s_wifi_handler = NULL;
static esp_event_handler_instance_t s_ip_handler = NULL;
static char s_ip[16] = {0};
static bool s_connected = false;
static bool s_initialized = false;

static wifi_prov_connected_cb_t s_on_connected = NULL;
static wifi_prov_failed_cb_t s_on_failed = NULL;

// NVS keys for WiFi credentials
#define NVS_NAMESPACE "wifi_config"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "password"
#define NVS_KEY_SERVER_IP "server_ip"
#define NVS_KEY_SERVER_PORT "server_port"

// Default credentials (will be overridden by NVS)
static char s_ssid[33] = "default";
static char s_password[65] = "default";
static char s_server_ip[16] = "192.168.1.100";
static uint16_t s_server_port = 9527;

// WiFi event handler
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started");
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "WiFi disconnected, reason=%u", event->reason);
        s_connected = false;
        // Try to reconnect
        esp_wifi_connect();
    }
}

// IP event handler
static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;

    ip_event_got_ip_t *event = data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
    s_connected = true;

    ESP_LOGI(TAG, "WiFi connected, IP: %s", s_ip);

    if (s_on_connected) {
        s_on_connected(s_ip);
    }
}

// Load WiFi config from NVS
static esp_err_t load_wifi_config(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No WiFi config in NVS, using defaults");
        return ESP_OK;
    }

    size_t len = sizeof(s_ssid);
    err = nvs_get_str(handle, NVS_KEY_SSID, s_ssid, &len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No SSID in NVS");
    }

    len = sizeof(s_password);
    err = nvs_get_str(handle, NVS_KEY_PASS, s_password, &len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No password in NVS");
    }

    len = sizeof(s_server_ip);
    err = nvs_get_str(handle, NVS_KEY_SERVER_IP, s_server_ip, &len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No server IP in NVS");
    }

    uint16_t port = 0;
    err = nvs_get_u16(handle, NVS_KEY_SERVER_PORT, &port);
    if (err == ESP_OK && port > 0) {
        s_server_port = port;
    }

    nvs_close(handle);

    ESP_LOGI(TAG, "Loaded WiFi config: SSID=%s, Server=%s:%d",
             s_ssid, s_server_ip, s_server_port);

    return ESP_OK;
}

// Save WiFi config to NVS
esp_err_t hermes_wifi_prov_save_config(const char *ssid, const char *password,
                                        const char *server_ip, uint16_t server_port)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (err != ESP_OK) goto done;

    err = nvs_set_str(handle, NVS_KEY_PASS, password);
    if (err != ESP_OK) goto done;

    err = nvs_set_str(handle, NVS_KEY_SERVER_IP, server_ip);
    if (err != ESP_OK) goto done;

    err = nvs_set_u16(handle, NVS_KEY_SERVER_PORT, server_port);
    if (err != ESP_OK) goto done;

    err = nvs_commit(handle);
    if (err != ESP_OK) goto done;

    // Update local state
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    strncpy(s_password, password, sizeof(s_password) - 1);
    strncpy(s_server_ip, server_ip, sizeof(s_server_ip) - 1);
    s_server_port = server_port;

    ESP_LOGI(TAG, "Saved WiFi config: SSID=%s, Server=%s:%d",
             s_ssid, s_server_ip, s_server_port);

done:
    nvs_close(handle);
    return err;
}

esp_err_t hermes_wifi_prov_start(wifi_prov_connected_cb_t on_connected,
                                  wifi_prov_failed_cb_t on_failed)
{
    s_on_connected = on_connected;
    s_on_failed = on_failed;

    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition issue, erasing...");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Load WiFi config from NVS
    load_wifi_config();

    // Initialize network
    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Netif init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop failed: %s", esp_err_to_name(err));
        return err;
    }

    // Create WiFi STA netif
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        ESP_LOGE(TAG, "Failed to create STA netif");
        return ESP_ERR_NO_MEM;
    }

    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_initialized = true;

    // Set WiFi mode to STA
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi set mode failed: %s", esp_err_to_name(err));
        return err;
    }

    // Set WiFi config
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, s_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, s_password, sizeof(wifi_config.sta.password) - 1);

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi set config failed: %s", esp_err_to_name(err));
        return err;
    }

    // Register event handlers
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event, NULL, &s_wifi_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi event handler failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &ip_event, NULL, &s_ip_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IP event handler failed: %s", esp_err_to_name(err));
        return err;
    }

    // Start WiFi
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WiFi STA started, connecting to %s...", s_ssid);

    return ESP_OK;
}

void hermes_wifi_prov_stop(void)
{
    ESP_LOGI(TAG, "Stopping WiFi");

    if (s_wifi_handler) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
        s_wifi_handler = NULL;
    }
    if (s_ip_handler) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler);
        s_ip_handler = NULL;
    }

    if (s_initialized) {
        esp_wifi_stop();
        esp_wifi_deinit();
        s_initialized = false;
    }

    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    s_connected = false;
}

bool hermes_wifi_prov_is_connected(void)
{
    return s_connected;
}

const char *hermes_wifi_prov_get_ip(void)
{
    return s_ip;
}

const char *hermes_wifi_prov_get_server_ip(void)
{
    return s_server_ip;
}

uint16_t hermes_wifi_prov_get_server_port(void)
{
    return s_server_port;
}
