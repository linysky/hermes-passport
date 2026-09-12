// main/hermes_ws_client.h — WebSocket client for streaming responses
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

// Callback for received messages
typedef void (*ws_message_cb_t)(const char *data, size_t len);

// Initialize WebSocket client
esp_err_t hermes_ws_init(const char *server_ip, uint16_t port);

// Connect to WebSocket server
esp_err_t hermes_ws_connect(void);

// Disconnect from WebSocket server
void hermes_ws_disconnect(void);

// Send a message
esp_err_t hermes_ws_send(const char *message);

// Send binary data (audio chunks)
esp_err_t hermes_ws_send_binary(const uint8_t *data, size_t len);

// Check if connected
bool hermes_ws_is_connected(void);

// Set message callback
void hermes_ws_set_callback(ws_message_cb_t callback);
