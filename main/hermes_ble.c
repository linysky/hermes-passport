// main/hermes_ble.c — BLE GATT client for hermes-passport (NimBLE)
// Communicates with Hermes Desktop Plugin companion via BLE
#include "hermes_ble.h"

#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "hermes_ble";

// ── UUID Definitions ───────────────────────────────────────────────────────

// Service UUID: 48450001-51c4-499d-a186-4621a4938301
// BLE_UUID128_INIT uses reversed byte order
static const ble_uuid128_t hermes_service_uuid =
    BLE_UUID128_INIT(0x01, 0x83, 0x93, 0xa4, 0x21, 0x46, 0x86, 0xa1,
                     0x9d, 0x49, 0xc4, 0x51, 0x01, 0x00, 0x45, 0x48);

// RX characteristic UUID: 48450002-51c4-499d-a186-4621a4938301
static const ble_uuid128_t hermes_rx_uuid =
    BLE_UUID128_INIT(0x02, 0x83, 0x93, 0xa4, 0x21, 0x46, 0x86, 0xa1,
                     0x9d, 0x49, 0xc4, 0x51, 0x01, 0x00, 0x45, 0x48);

// TX characteristic UUID: 48450003-51c4-499d-a186-4621a4938301
static const ble_uuid128_t hermes_tx_uuid =
    BLE_UUID128_INIT(0x03, 0x83, 0x93, 0xa4, 0x21, 0x46, 0x86, 0xa1,
                     0x9d, 0x49, 0xc4, 0x51, 0x01, 0x00, 0x45, 0x48);

// VOICE characteristic UUID: 48450004-51c4-499d-a186-4621a4938301
static const ble_uuid128_t hermes_voice_uuid =
    BLE_UUID128_INIT(0x04, 0x83, 0x93, 0xa4, 0x21, 0x46, 0x86, 0xa1,
                     0x9d, 0x49, 0xc4, 0x51, 0x01, 0x00, 0x45, 0x48);

// ── Message Types ──────────────────────────────────────────────────────────

#define MSG_TYPE_BOT_LIST       0x01
#define MSG_TYPE_STREAM_START   0x02
#define MSG_TYPE_STREAM_CHUNK   0x03
#define MSG_TYPE_STREAM_END     0x04
#define MSG_TYPE_STT_RESULT     0x05
#define MSG_TYPE_STT_ERROR      0x06
#define MSG_TYPE_HEARTBEAT      0x07
#define MSG_TYPE_BOT_STATUS     0x08

#define MSG_TYPE_SEND_TEXT      0x01
#define MSG_TYPE_SEND_QUICK     0x02
#define MSG_TYPE_AUDIO_START    0x03
#define MSG_TYPE_AUDIO_END      0x04
#define MSG_TYPE_OPEN_BOT       0x05
#define MSG_TYPE_SCROLL_PAGE    0x06
#define MSG_TYPE_HEARTBEAT_ACK  0x07

#define VOICE_KIND_START    1
#define VOICE_KIND_DATA     2
#define VOICE_KIND_END      3
#define VOICE_KIND_CANCEL   4

// ── State ──────────────────────────────────────────────────────────────────

static hermes_ble_state_t s_state = HERMES_BLE_DISCONNECTED;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_val_handle = 0;
static uint16_t s_rx_val_handle = 0;
static uint16_t s_voice_val_handle = 0;
static uint16_t s_pairing_code = 0;

// Callbacks
static hermes_ble_on_bots_cb_t s_on_bots = NULL;
static hermes_ble_on_stream_chunk_cb_t s_on_stream_chunk = NULL;
static hermes_ble_on_stream_end_cb_t s_on_stream_end = NULL;
static hermes_ble_on_stt_result_cb_t s_on_stt_result = NULL;
static hermes_ble_on_stt_error_cb_t s_on_stt_error = NULL;
static hermes_ble_on_connected_cb_t s_on_connected = NULL;
static hermes_ble_on_disconnected_cb_t s_on_disconnected = NULL;

// NVS
#define NVS_NAMESPACE "hermes_ble"
#define NVS_KEY_PAIRED "paired"

// ── Forward Declarations ───────────────────────────────────────────────────

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg);
static void ble_on_reset(int reason);
static void ble_on_sync(void);
static void ble_host_task(void *param);

// ── GATT Service Definition ────────────────────────────────────────────────

static const struct ble_gatt_chr_def gatt_chars[] = {
    {
        .uuid = &hermes_rx_uuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_AUTHEN,
        .val_handle = &s_rx_val_handle,
    },
    {
        .uuid = &hermes_tx_uuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_AUTHEN,
        .val_handle = &s_tx_val_handle,
    },
    {
        .uuid = &hermes_voice_uuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_AUTHEN,
        .val_handle = &s_voice_val_handle,
    },
    { 0 },  // End
};

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &hermes_service_uuid.u,
        .characteristics = gatt_chars,
    },
    { 0 },  // End
};

// ── GATT Access Callback ───────────────────────────────────────────────────

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (attr_handle == s_rx_val_handle) {
            // RX write from companion
            const uint8_t *data = ctxt->om->om_data;
            size_t len = ctxt->om->om_len;
            // handle_rx_message(data, len);
        } else if (attr_handle == s_voice_val_handle) {
            // Voice data from companion (should not happen, device sends voice)
        }
    }
    return 0;
}

// ── GAP Event Handler ──────────────────────────────────────────────────────

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "BLE connected, handle=%d", event->connect.conn_handle);
            s_conn_handle = event->connect.conn_handle;
            s_state = HERMES_BLE_CONNECTED;
            if (s_on_connected) s_on_connected();
        } else {
            ESP_LOGE(TAG, "BLE connect failed, status=%d", event->connect.status);
            s_state = HERMES_BLE_ADVERTISING;
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "BLE disconnected, reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_state = HERMES_BLE_ADVERTISING;
        if (s_on_disconnected) s_on_disconnected();
        // Restart advertising
        ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                          &(struct ble_gap_adv_params){
                              .conn_mode = BLE_GAP_CONN_MODE_UND,
                              .disc_mode = BLE_GAP_DISC_MODE_GEN,
                          }, gap_event_handler, NULL);
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "Subscribe event, attr_handle=%d", event->subscribe.attr_handle);
        break;

    default:
        break;
    }
    return 0;
}

// ── NimBLE Callbacks ───────────────────────────────────────────────────────

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE reset, reason=%d", reason);
}

static void ble_on_sync(void)
{
    ESP_LOGI(TAG, "BLE synced");

    // Ensure address
    int rc = ble_hs_util_ensure_addr(0);
    if (rc) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: %d", rc);
        return;
    }

    // Infer address type
    uint8_t addr_type;
    rc = ble_hs_id_infer_auto(0, &addr_type);
    if (rc) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }

    // Set device name
    ble_svc_gap_device_name_set("Hermes-Passport");

    // Advertising data: flags + service UUID
    struct ble_hs_adv_fields adv_fields = {0};
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv_fields.uuids128 = (ble_uuid128_t *)&hermes_service_uuid;
    adv_fields.num_uuids128 = 1;
    adv_fields.uuids128_is_complete = 1;
    rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return;
    }

    // Scan response data: device name
    struct ble_hs_adv_fields scan_rsp = {0};
    scan_rsp.name = (const uint8_t *)"Hermes-Passport";
    scan_rsp.name_len = 15;
    scan_rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&scan_rsp);
    if (rc) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields failed: %d", rc);
        return;
    }

    // Start advertising
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(addr_type, NULL, BLE_HS_FOREVER, &params, gap_event_handler, NULL);
    if (rc) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
        return;
    }

    s_state = HERMES_BLE_ADVERTISING;
    ESP_LOGI(TAG, "Advertising started, pairing code: %04d", s_pairing_code);
}

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ── Public API ─────────────────────────────────────────────────────────────

esp_err_t hermes_ble_init(void)
{
    // Release classic BT memory
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Set callbacks
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_bonding = 1;

    // Initialize GATT services
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(gatt_svcs);
    ble_gatts_add_svcs(gatt_svcs);

    // Generate pairing code
    s_pairing_code = 1000 + (esp_random() % 9000);

    ESP_LOGI(TAG, "BLE initialized");
    return ESP_OK;
}

esp_err_t hermes_ble_start(void)
{
    nimble_port_freertos_init(ble_host_task);
    return ESP_OK;
}

void hermes_ble_stop(void)
{
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    nimble_port_stop();
    nimble_port_deinit();
    s_state = HERMES_BLE_DISCONNECTED;
}

hermes_ble_state_t hermes_ble_get_state(void) { return s_state; }
uint16_t hermes_ble_get_pairing_code(void) { return s_pairing_code; }

// ── Send Messages ──────────────────────────────────────────────────────────

esp_err_t hermes_ble_send_text(const char *bot_id, const char *text)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return ESP_ERR_INVALID_STATE;

    uint8_t msg[256];
    size_t text_len = strlen(text);
    size_t offset = 0;

    msg[offset++] = 1;  // version
    msg[offset++] = MSG_TYPE_SEND_TEXT;
    memset(msg + offset, 0, 40);
    strncpy((char *)msg + offset, bot_id, 40);
    offset += 40;

    char req_id[37];
    snprintf(req_id, 37, "%08x", (unsigned)esp_random());
    memcpy(msg + offset, req_id, 36);
    offset += 36;

    msg[offset++] = text_len & 0xFF;
    msg[offset++] = (text_len >> 8) & 0xFF;
    memcpy(msg + offset, text, text_len);
    offset += text_len;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(msg, offset);
    return ble_gattc_notify_custom(s_conn_handle, s_tx_val_handle, om);
}

esp_err_t hermes_ble_send_quick(const char *bot_id, uint8_t action)
{
    uint8_t msg[82];
    size_t offset = 0;
    msg[offset++] = 1;
    msg[offset++] = MSG_TYPE_SEND_QUICK;
    memset(msg + offset, 0, 40);
    strncpy((char *)msg + offset, bot_id, 40);
    offset += 40;
    char req_id[37];
    snprintf(req_id, 37, "%08x", (unsigned)esp_random());
    memcpy(msg + offset, req_id, 36);
    offset += 36;
    msg[offset++] = action;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(msg, offset);
    return ble_gattc_notify_custom(s_conn_handle, s_tx_val_handle, om);
}

esp_err_t hermes_ble_audio_start(const char *bot_id)
{
    ESP_LOGI(TAG, "Audio start for bot: %s", bot_id);
    // TODO: Send via VOICE characteristic
    return ESP_OK;
}

esp_err_t hermes_ble_audio_frame(const uint8_t *opus_data, size_t len, uint16_t sequence)
{
    ESP_LOGD(TAG, "Audio frame: %d bytes, seq=%d", len, sequence);
    // TODO: Send Opus frame via VOICE characteristic
    return ESP_OK;
}

esp_err_t hermes_ble_audio_end(void)
{
    ESP_LOGI(TAG, "Audio end");
    // TODO: Send audio end via VOICE characteristic
    return ESP_OK;
}

esp_err_t hermes_ble_audio_cancel(void)
{
    ESP_LOGI(TAG, "Audio cancel");
    return ESP_OK;
}

esp_err_t hermes_ble_open_bot(const char *bot_id)
{
    uint8_t msg[42];
    msg[0] = 1;
    msg[1] = MSG_TYPE_OPEN_BOT;
    memset(msg + 2, 0, 40);
    strncpy((char *)msg + 2, bot_id, 40);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(msg, 42);
    return ble_gattc_notify_custom(s_conn_handle, s_tx_val_handle, om);
}

esp_err_t hermes_ble_scroll_page(uint8_t direction)
{
    uint8_t msg[3] = {1, MSG_TYPE_SCROLL_PAGE, direction};
    struct os_mbuf *om = ble_hs_mbuf_from_flat(msg, 3);
    return ble_gattc_notify_custom(s_conn_handle, s_tx_val_handle, om);
}

// ── Callbacks ──────────────────────────────────────────────────────────────

void hermes_ble_set_callbacks(
    hermes_ble_on_bots_cb_t on_bots,
    hermes_ble_on_stream_chunk_cb_t on_stream_chunk,
    hermes_ble_on_stream_end_cb_t on_stream_end,
    hermes_ble_on_stt_result_cb_t on_stt_result,
    hermes_ble_on_stt_error_cb_t on_stt_error,
    hermes_ble_on_connected_cb_t on_connected,
    hermes_ble_on_disconnected_cb_t on_disconnected)
{
    s_on_bots = on_bots;
    s_on_stream_chunk = on_stream_chunk;
    s_on_stream_end = on_stream_end;
    s_on_stt_result = on_stt_result;
    s_on_stt_error = on_stt_error;
    s_on_connected = on_connected;
    s_on_disconnected = on_disconnected;
}

// ── NVS Pairing ────────────────────────────────────────────────────────────

const char *hermes_ble_get_paired_name(void)
{
    static char name[64] = {0};
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(name);
        nvs_get_str(handle, NVS_KEY_PAIRED, name, &len);
        nvs_close(handle);
    }
    return name;
}

esp_err_t hermes_ble_clear_pairing(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    nvs_erase_all(handle);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Pairing cleared");
    return ESP_OK;
}
