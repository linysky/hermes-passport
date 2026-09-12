// main/hermes_wifi_prov.c — WiFi provisioning via BLUFI for hermes-bridge
// Uses BLE to receive WiFi credentials from ESP Config mobile app
//
// Flow:
//   1. ESP32 starts BLE advertising as "BLUFI_HermesPassport"
//   2. User opens ESP Config app on phone, connects to device
//   3. App scans WiFi networks, user selects one and enters password
//   4. Credentials are sent to ESP32 via BLUFI protocol
//   5. ESP32 connects to WiFi, reports IP back to app
//   6. User can then configure the plugin server address

#include "hermes_wifi_prov.h"
#include "demo_radio.h"

#include "esp_blufi.h"
#include "esp_blufi_api.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#include <string.h>

static const char *TAG = "wifi_prov";

// BLUFI device name — ESP Config app looks for "BLUFI" prefix
static const char *DEVICE_NAME = "BLUFI_HermesPassport";

#define WIFI_SCAN_MAX_AP 16

// ── State ──────────────────────────────────────────────────────────────────

typedef enum {
    PROV_OFF = 0,
    PROV_STARTING,
    PROV_ADVERTISING,
    PROV_BLE_CONNECTED,
    PROV_WIFI_CONNECTING,
    PROV_WIFI_CONNECTED,
    PROV_FAILED,
} prov_state_t;

static volatile prov_state_t s_state = PROV_OFF;
static volatile esp_err_t s_error = ESP_OK;

static esp_netif_t *s_sta_netif = NULL;
static esp_event_handler_instance_t s_wifi_handler = NULL;
static esp_event_handler_instance_t s_ip_handler = NULL;
static wifi_config_t s_sta_config = {0};
static char s_ip[16] = {0};

static bool s_wifi_initialized = false;
static bool s_wifi_started = false;
static bool s_wifi_handler_registered = false;
static bool s_ip_handler_registered = false;
static bool s_host_initialized = false;
static bool s_host_running = false;
static bool s_gatt_initialized = false;
static bool s_profile_initialized = false;
static bool s_ble_connected = false;
static bool s_wifi_connecting = false;
static bool s_wifi_got_ip = false;

static SemaphoreHandle_t s_host_stopped = NULL;
static wifi_prov_connected_cb_t s_on_connected = NULL;
static wifi_prov_failed_cb_t s_on_failed = NULL;

// ── BLUFI Callbacks ────────────────────────────────────────────────────────

static void send_wifi_report(esp_blufi_sta_conn_state_t state)
{
    wifi_mode_t mode = WIFI_MODE_STA;
    esp_wifi_get_mode(&mode);
    esp_blufi_extra_info_t info = {0};
    size_t ssid_len = strnlen((const char *)s_sta_config.sta.ssid,
                              sizeof(s_sta_config.sta.ssid));
    if (ssid_len > 0) {
        info.sta_ssid = s_sta_config.sta.ssid;
        info.sta_ssid_len = ssid_len;
    }
    esp_blufi_send_wifi_conn_report(mode, state, 0, &info);
}

static void send_wifi_list(void)
{
    uint16_t count = WIFI_SCAN_MAX_AP;
    wifi_ap_record_t records[WIFI_SCAN_MAX_AP] = {0};
    esp_blufi_ap_record_t list[WIFI_SCAN_MAX_AP] = {0};

    esp_err_t err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        return;
    }

    for (uint16_t i = 0; i < count; i++) {
        list[i].rssi = records[i].rssi;
        memcpy(list[i].ssid, records[i].ssid, sizeof(list[i].ssid));
    }

    if (s_ble_connected) {
        esp_blufi_send_wifi_list(count, list);
    }
}

static void request_wifi_connect(void)
{
    bool was_connected = s_wifi_got_ip;
    s_wifi_connecting = true;
    s_wifi_got_ip = false;
    s_state = PROV_WIFI_CONNECTING;

    if (was_connected) {
        if (esp_wifi_disconnect() == ESP_OK) return;
    }

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_error = err;
        s_wifi_connecting = false;
        s_state = PROV_FAILED;
    }
}

// ── WiFi Event Handlers ────────────────────────────────────────────────────

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_get_config(WIFI_IF_STA, &s_sta_config);
        if (s_sta_config.sta.ssid[0] != '\0') {
            s_wifi_connecting = true;
            s_state = PROV_WIFI_CONNECTING;
            esp_wifi_connect();
        }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "WiFi disconnected, reason=%u", event->reason);

        if (s_state == PROV_WIFI_CONNECTING) {
            send_wifi_report(ESP_BLUFI_STA_CONN_FAIL);
        }
        s_wifi_connecting = false;
        s_wifi_got_ip = false;
        s_state = s_ble_connected ? PROV_BLE_CONNECTED : PROV_ADVERTISING;
        s_ip[0] = '\0';

        if (s_on_failed) {
            s_on_failed(ESP_FAIL);
        }
    } else if (id == WIFI_EVENT_SCAN_DONE) {
        send_wifi_list();
    }
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;

    ip_event_got_ip_t *event = data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
    s_wifi_connecting = false;
    s_wifi_got_ip = true;
    s_state = PROV_WIFI_CONNECTED;

    ESP_LOGI(TAG, "WiFi connected, IP: %s", s_ip);

    if (s_ble_connected) {
        send_wifi_report(ESP_BLUFI_STA_CONN_SUCCESS);
    }

    if (s_on_connected) {
        s_on_connected(s_ip);
    }
}

// ── BLUFI Event Handler ────────────────────────────────────────────────────

static void blufi_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset: %d", reason);
    s_error = ESP_FAIL;
    s_state = PROV_FAILED;
}

static void blufi_sync(void)
{
    int rc = esp_blufi_profile_init();
    if (rc == 0) {
        s_profile_initialized = true;
    } else {
        s_error = rc;
        s_state = PROV_FAILED;
    }
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    if (s_host_stopped) xSemaphoreGive(s_host_stopped);
    nimble_port_freertos_deinit();
}

static void blufi_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        ESP_LOGI(TAG, "BLUFI init finish");
        esp_blufi_adv_start_with_name(DEVICE_NAME);
        if (s_wifi_got_ip) {
            s_state = PROV_WIFI_CONNECTED;
        } else if (s_wifi_connecting) {
            s_state = PROV_WIFI_CONNECTING;
        } else {
            s_state = PROV_ADVERTISING;
        }
        break;

    case ESP_BLUFI_EVENT_BLE_CONNECT:
        ESP_LOGI(TAG, "BLE connected");
        s_ble_connected = true;
        s_state = PROV_BLE_CONNECTED;
        break;

    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected");
        s_ble_connected = false;
        if (s_wifi_got_ip) {
            s_state = PROV_WIFI_CONNECTED;
        } else {
            s_state = PROV_ADVERTISING;
        }
        esp_blufi_adv_start_with_name(DEVICE_NAME);
        break;

    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        ESP_LOGI(TAG, "Set WiFi mode: %d", param->wifi_mode.op_mode);
        esp_wifi_set_mode(param->wifi_mode.op_mode);
        break;

    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        ESP_LOGI(TAG, "Request connect to AP");
        request_wifi_connect();
        break;

    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
        ESP_LOGI(TAG, "Request disconnect from AP");
        esp_wifi_disconnect();
        break;

    case ESP_BLUFI_EVENT_REPORT_ERROR:
        ESP_LOGE(TAG, "BLUFI error: %d", param->report_error.state);
        s_error = param->report_error.state;
        s_state = PROV_FAILED;
        break;

    case ESP_BLUFI_EVENT_GET_WIFI_STATUS:
        ESP_LOGI(TAG, "Get WiFi status");
        send_wifi_report(s_wifi_got_ip ?
            ESP_BLUFI_STA_CONN_SUCCESS : ESP_BLUFI_STA_CONN_FAIL);
        break;

    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        ESP_LOGI(TAG, "Request BLE disconnect");
        esp_blufi_disconnect();
        break;

    case ESP_BLUFI_EVENT_RECV_STA_SSID:
        ESP_LOGI(TAG, "Received SSID: %.*s",
                 param->sta_ssid.ssid_len, param->sta_ssid.ssid);
        memset(s_sta_config.sta.ssid, 0, sizeof(s_sta_config.sta.ssid));
        memcpy(s_sta_config.sta.ssid, param->sta_ssid.ssid,
               param->sta_ssid.ssid_len);
        break;

    case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        ESP_LOGI(TAG, "Received password");
        memset(s_sta_config.sta.password, 0, sizeof(s_sta_config.sta.password));
        memcpy(s_sta_config.sta.password, param->sta_passwd.passwd,
               param->sta_passwd.passwd_len);
        break;

    case ESP_BLUFI_EVENT_GET_WIFI_LIST:
        ESP_LOGI(TAG, "Get WiFi list");
        esp_wifi_scan_start(NULL, false);
        break;

    default:
        ESP_LOGW(TAG, "Unhandled BLUFI event: %d", event);
        break;
    }
}

static esp_blufi_callbacks_t s_blufi_callbacks = {
    .event_cb = blufi_event,
    .negotiate_data_handler = NULL,  // No encryption for now
    .encrypt_func = NULL,
    .decrypt_func = NULL,
    .checksum_func = NULL,
};

// ── Public API ─────────────────────────────────────────────────────────────

esp_err_t hermes_wifi_prov_start(wifi_prov_connected_cb_t on_connected,
                                  wifi_prov_failed_cb_t on_failed)
{
    s_on_connected = on_connected;
    s_on_failed = on_failed;

    // Initialize NVS
    esp_err_t err = demo_radio_nvs_prepare();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Initialize network
    err = demo_radio_network_prepare();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Network init failed: %s", esp_err_to_name(err));
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
    s_wifi_initialized = true;

    // Set WiFi mode to STA
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi set mode failed: %s", esp_err_to_name(err));
        return err;
    }

    // Register WiFi event handler
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event, NULL, &s_wifi_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi event handler failed: %s", esp_err_to_name(err));
        return err;
    }
    s_wifi_handler_registered = true;

    // Register IP event handler
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &ip_event, NULL, &s_ip_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IP event handler failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ip_handler_registered = true;

    // Start WiFi
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_wifi_started = true;

    // Initialize NimBLE
    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_host_initialized = true;

    // Set device name
    ble_svc_gap_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);

    // Initialize BLUFI
    err = esp_blufi_register_callbacks(&s_blufi_callbacks);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLUFI register callbacks failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_blufi_profile_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLUFI profile init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_profile_initialized = true;

    // Start NimBLE host task
    s_host_stopped = xSemaphoreCreateBinary();
    nimble_port_freertos_init(host_task);
    s_host_running = true;

    s_state = PROV_STARTING;
    ESP_LOGI(TAG, "BLUFI provisioning started, device: %s", DEVICE_NAME);

    return ESP_OK;
}

void hermes_wifi_prov_stop(void)
{
    ESP_LOGI(TAG, "Stopping BLUFI provisioning");

    // Stop BLUFI advertising
    if (s_profile_initialized) {
        esp_blufi_profile_deinit();
        s_profile_initialized = false;
    }

    // Stop NimBLE
    if (s_host_running) {
        esp_blufi_disconnect();
        nimble_port_stop();
        if (s_host_stopped) {
            xSemaphoreTake(s_host_stopped, pdMS_TO_TICKS(5000));
        }
        s_host_running = false;
    }

    if (s_host_initialized) {
        nimble_port_deinit();
        s_host_initialized = false;
    }

    // Unregister event handlers
    if (s_ip_handler_registered) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler);
        s_ip_handler_registered = false;
    }
    if (s_wifi_handler_registered) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
        s_wifi_handler_registered = false;
    }

    // Stop WiFi
    if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_initialized) {
        esp_wifi_deinit();
        s_wifi_initialized = false;
    }

    // Destroy netif
    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    s_state = PROV_OFF;
    s_ble_connected = false;
    s_wifi_connecting = false;
    s_wifi_got_ip = false;
}

bool hermes_wifi_prov_is_connected(void)
{
    return s_wifi_got_ip;
}

const char *hermes_wifi_prov_get_ip(void)
{
    return s_ip;
}
