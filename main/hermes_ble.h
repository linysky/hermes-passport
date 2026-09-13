// main/hermes_ble.h — BLE GATT client for hermes-passport (NimBLE)
// Communicates with Hermes Desktop Plugin companion via BLE
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// BLE connection state
typedef enum {
    HERMES_BLE_DISCONNECTED = 0,
    HERMES_BLE_ADVERTISING,
    HERMES_BLE_CONNECTED,
    HERMES_BLE_PAIRED,
} hermes_ble_state_t;

// Bot info received from companion
typedef struct {
    char id[41];
    char name[81];
    char status[17];
    char description[193];
} hermes_ble_bot_t;

#define HERMES_BLE_MAX_BOTS 8

// Callbacks
typedef void (*hermes_ble_on_bots_cb_t)(const hermes_ble_bot_t *bots, int count);
typedef void (*hermes_ble_on_stream_chunk_cb_t)(const char *request_id, const char *content);
typedef void (*hermes_ble_on_stream_end_cb_t)(const char *request_id);
typedef void (*hermes_ble_on_stt_result_cb_t)(const char *request_id, const char *text);
typedef void (*hermes_ble_on_stt_error_cb_t)(const char *request_id, const char *error);
typedef void (*hermes_ble_on_connected_cb_t)(void);
typedef void (*hermes_ble_on_disconnected_cb_t)(void);

// Initialize BLE and start advertising
esp_err_t hermes_ble_init(void);

// Start BLE
esp_err_t hermes_ble_start(void);

// Stop BLE
void hermes_ble_stop(void);

// Get current state
hermes_ble_state_t hermes_ble_get_state(void);

// Send text message to a bot
esp_err_t hermes_ble_send_text(const char *bot_id, const char *text);

// Send quick action (1=继续, 2=stop)
esp_err_t hermes_ble_send_quick(const char *bot_id, uint8_t action);

// Start audio recording
esp_err_t hermes_ble_audio_start(const char *bot_id);

// Send Opus audio frame
esp_err_t hermes_ble_audio_frame(const uint8_t *opus_data, size_t len, uint16_t sequence);

// End audio recording
esp_err_t hermes_ble_audio_end(void);

// Cancel audio recording
esp_err_t hermes_ble_audio_cancel(void);

// Open a bot
esp_err_t hermes_ble_open_bot(const char *bot_id);

// Send scroll page
esp_err_t hermes_ble_scroll_page(uint8_t direction);

// Register callbacks
void hermes_ble_set_callbacks(
    hermes_ble_on_bots_cb_t on_bots,
    hermes_ble_on_stream_chunk_cb_t on_stream_chunk,
    hermes_ble_on_stream_end_cb_t on_stream_end,
    hermes_ble_on_stt_result_cb_t on_stt_result,
    hermes_ble_on_stt_error_cb_t on_stt_error,
    hermes_ble_on_connected_cb_t on_connected,
    hermes_ble_on_disconnected_cb_t on_disconnected
);

// Get paired device name
const char *hermes_ble_get_paired_name(void);

// Clear pairing info
esp_err_t hermes_ble_clear_pairing(void);

// Get pairing code
uint16_t hermes_ble_get_pairing_code(void);
