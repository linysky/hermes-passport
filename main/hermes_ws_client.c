// main/hermes_ws_client.c — WebSocket client for streaming responses
// Connects to Desktop plugin for real-time message streaming
#include "hermes_ws_client.h"

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_event.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "ws_client";

static esp_websocket_client_handle_t s_client = NULL;
static ws_message_cb_t s_callback = NULL;
static bool s_connected = false;
static char s_server_ip[16] = {0};
static uint16_t s_server_port = 0;

static void websocket_event_handler(void *arg, esp_event_base_t base,
                                     int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        s_connected = true;
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        s_connected = false;
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x01) {  // Text frame
            ESP_LOGI(TAG, "Received text: %.*s", data->data_len, data->data_ptr);
            if (s_callback && data->data_len > 0) {
                // Null-terminate the string
                char *msg = malloc(data->data_len + 1);
                if (msg) {
                    memcpy(msg, data->data_ptr, data->data_len);
                    msg[data->data_len] = '\0';
                    s_callback(msg, data->data_len);
                    free(msg);
                }
            }
        } else if (data->op_code == 0x02) {  // Binary frame
            ESP_LOGI(TAG, "Received binary: %d bytes", data->data_len);
            // Handle binary data (TTS audio, etc.)
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;

    default:
        break;
    }
}

esp_err_t hermes_ws_init(const char *server_ip, uint16_t port)
{
    if (!server_ip || port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_server_ip, server_ip, sizeof(s_server_ip) - 1);
    s_server_port = port;

    ESP_LOGI(TAG, "WebSocket client initialized: %s:%d", s_server_ip, s_server_port);
    return ESP_OK;
}

esp_err_t hermes_ws_connect(void)
{
    if (s_client) {
        ESP_LOGW(TAG, "WebSocket already connected");
        return ESP_OK;
    }

    // Build WebSocket URI
    char uri[64];
    snprintf(uri, sizeof(uri), "ws://%s:%d/ws", s_server_ip, s_server_port);

    esp_websocket_client_config_t config = {
        .uri = uri,
        .task_stack = 4096,
        .buffer_size = 4096,
    };

    s_client = esp_websocket_client_init(&config);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        return ESP_FAIL;
    }

    // Register event handler
    esp_err_t err = esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                                    websocket_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register events: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    // Start connection
    err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    ESP_LOGI(TAG, "WebSocket connecting to %s", uri);
    return ESP_OK;
}

void hermes_ws_disconnect(void)
{
    if (s_client) {
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        s_connected = false;
        ESP_LOGI(TAG, "WebSocket disconnected");
    }
}

esp_err_t hermes_ws_send(const char *message)
{
    if (!s_client || !s_connected) {
        ESP_LOGE(TAG, "WebSocket not connected");
        return ESP_ERR_INVALID_STATE;
    }

    int sent = esp_websocket_client_send_text(s_client, message, strlen(message), pdMS_TO_TICKS(1000));
    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send message");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sent message: %d bytes", sent);
    return ESP_OK;
}

esp_err_t hermes_ws_send_binary(const uint8_t *data, size_t len)
{
    if (!s_client || !s_connected) {
        ESP_LOGE(TAG, "WebSocket not connected");
        return ESP_ERR_INVALID_STATE;
    }

    int sent = esp_websocket_client_send_bin(s_client, (const char *)data, len, pdMS_TO_TICKS(1000));
    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send binary");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Sent binary: %d bytes", sent);
    return ESP_OK;
}

bool hermes_ws_is_connected(void)
{
    return s_connected;
}

void hermes_ws_set_callback(ws_message_cb_t callback)
{
    s_callback = callback;
}
