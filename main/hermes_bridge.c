// main/hermes_bridge.c — Hermes Bridge client for AI Passport
// BLE-based client connecting to Hermes Desktop Plugin companion
//
// Architecture:
//   ESP32 --BLE--> Desktop Plugin --JSON-RPC--> Hermes Gateway
//
// Key design:
//   - rebuild_*_ui() NEVER acquire LVGL lock (caller must hold it)
//   - BLE callbacks use set_state() which locks internally
//   - Key handler called from on_key() which already holds lock
//   - DOWN long press = back to bot list (OK long press = global exit in main.c)

#include "hermes_bridge.h"
#include "hermes_ble.h"
#include "demo.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "ui_pixel.h"

#include "esp_log.h"
#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "hermes_bridge";

// ── UI Colors ──────────────────────────────────────────────────────────────

#define COLOR_BG            0x000000
#define COLOR_USER_MSG      0x1a73e8
#define COLOR_BOT_MSG       0x2d2d2d
#define COLOR_TEXT          0xffffff
#define COLOR_TEXT_DIM      0x9e9e9e
#define COLOR_HIGHLIGHT     0x3949ab
#define COLOR_ACCENT        0x4caf50
#define COLOR_ERROR         0xf44336

// ── Constants ──────────────────────────────────────────────────────────────

#define MAX_BOTS            8
#define MAX_MSG_TEXT        256
#define MAX_MESSAGES        20
#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_TASK_STACK    4096
#define OPUS_FRAME_SAMPLES  640

// ── UI States ──────────────────────────────────────────────────────────────

typedef enum {
    STATE_PAIRING = 0,
    STATE_CONNECTING,
    STATE_BOT_LIST,
    STATE_CHAT_INPUT,
    STATE_CHAT_SCROLL,
    STATE_RECORDING,
    STATE_TRANSCRIBE,
    STATE_ERROR,
} ui_state_t;

// ── Chat Message ───────────────────────────────────────────────────────────

typedef struct {
    char text[MAX_MSG_TEXT];
    bool is_user;
    bool is_system;
} chat_message_t;

// ── Global State ───────────────────────────────────────────────────────────

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_content_panel = NULL;
static lv_obj_t *s_hint_label = NULL;

static ui_state_t s_state = STATE_PAIRING;
static TaskHandle_t s_audio_task = NULL;

static hermes_ble_bot_t s_bots[MAX_BOTS];
static int s_bot_count = 0;
static int s_bot_selected = 0;

static chat_message_t s_messages[MAX_MESSAGES];
static int s_msg_count = 0;
static int s_msg_head = 0;

static volatile bool s_recording = false;
static volatile bool s_audio_stop = false;
static uint16_t s_audio_sequence = 0;

static char s_transcribe_text[MAX_MSG_TEXT] = {0};
static int s_transcribe_pages = 0;
static int s_transcribe_current_page = 0;

// ── Forward Declarations ───────────────────────────────────────────────────

static void set_state(ui_state_t new_state);
static void rebuild_ui(void);
static void add_message(const char *text, bool is_user, bool is_system);

// ── UI Helpers ─────────────────────────────────────────────────────────────
// NOTE: These do NOT lock LVGL — caller must hold the lock.

static void rebuild_bot_list_ui(void) {
    if (s_content_panel) {
        lv_obj_delete(s_content_panel);
        s_content_panel = NULL;
    }

    s_content_panel = lv_obj_create(s_scr);
    lv_obj_set_size(s_content_panel, 204, 220);
    lv_obj_align(s_content_panel, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_bg_color(s_content_panel, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_border_width(s_content_panel, 0, 0);
    lv_obj_set_style_pad_all(s_content_panel, 4, 0);

    if (s_bot_count == 0) {
        lv_obj_t *empty = lv_label_create(s_content_panel);
        lv_label_set_text(empty, "No bots available");
        lv_obj_set_style_text_color(empty, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_center(empty);
        return;
    }

    for (int i = 0; i < s_bot_count; i++) {
        lv_obj_t *row = lv_obj_create(s_content_panel);
        lv_obj_set_size(row, 196, 44);
        lv_obj_set_style_bg_color(row,
            lv_color_hex(i == s_bot_selected ? COLOR_HIGHLIGHT : COLOR_BG), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_pos(row, 0, i * 48);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, s_bots[i].name);
        lv_obj_set_style_text_color(name, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 4, -6);

        lv_obj_t *desc = lv_label_create(row);
        lv_label_set_text(desc, s_bots[i].description);
        lv_obj_set_style_text_color(desc, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_set_style_text_font(desc, &lv_font_montserrat_12, 0);
        lv_obj_align(desc, LV_ALIGN_LEFT_MID, 4, 8);
    }
}

static void rebuild_chat_ui(void) {
    if (s_content_panel) {
        lv_obj_delete(s_content_panel);
        s_content_panel = NULL;
    }

    s_content_panel = lv_obj_create(s_scr);
    lv_obj_set_size(s_content_panel, 204, 220);
    lv_obj_align(s_content_panel, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_bg_color(s_content_panel, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_border_width(s_content_panel, 0, 0);
    lv_obj_set_style_pad_all(s_content_panel, 4, 0);
    lv_obj_set_flex_flow(s_content_panel, LV_FLEX_FLOW_COLUMN);

    if (s_msg_count == 0) {
        lv_obj_t *empty = lv_label_create(s_content_panel);
        lv_label_set_text(empty, "No messages yet\n\nOK: voice\nUP: continue\nDOWN: /stop");
        lv_obj_set_style_text_color(empty, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(empty);
        return;
    }

    int start = (s_msg_count < MAX_MESSAGES) ? 0 :
                (s_msg_head + 1) % MAX_MESSAGES;
    int count = s_msg_count;

    for (int i = 0; i < count; i++) {
        int idx = (start + i) % MAX_MESSAGES;
        chat_message_t *msg = &s_messages[idx];

        lv_obj_t *bubble = lv_obj_create(s_content_panel);
        lv_obj_set_width(bubble, 190);
        lv_obj_set_style_radius(bubble, 4, 0);
        lv_obj_set_style_pad_all(bubble, 4, 0);

        if (msg->is_system) {
            lv_obj_set_style_bg_opa(bubble, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(bubble, 0, 0);
        } else if (msg->is_user) {
            lv_obj_set_style_bg_color(bubble, lv_color_hex(COLOR_USER_MSG), 0);
            lv_obj_set_style_border_width(bubble, 0, 0);
            lv_obj_set_style_margin_left(bubble, 20, 0);
        } else {
            lv_obj_set_style_bg_color(bubble, lv_color_hex(COLOR_BOT_MSG), 0);
            lv_obj_set_style_border_width(bubble, 0, 0);
            lv_obj_set_style_margin_right(bubble, 20, 0);
        }

        lv_obj_t *text = lv_label_create(bubble);
        lv_label_set_text(text, msg->text);
        lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(text, 178);
        lv_obj_set_style_text_color(text, lv_color_hex(
            msg->is_system ? COLOR_TEXT_DIM : COLOR_TEXT), 0);
    }
}

static void rebuild_transcribe_ui(void) {
    if (s_content_panel) {
        lv_obj_delete(s_content_panel);
        s_content_panel = NULL;
    }

    s_content_panel = lv_obj_create(s_scr);
    lv_obj_set_size(s_content_panel, 204, 220);
    lv_obj_align(s_content_panel, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_bg_color(s_content_panel, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_border_width(s_content_panel, 0, 0);
    lv_obj_set_style_pad_all(s_content_panel, 8, 0);

    lv_obj_t *title = lv_label_create(s_content_panel);
    lv_label_set_text(title, "Transcription:");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *text = lv_label_create(s_content_panel);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(text, 188);
    lv_obj_set_style_text_color(text, lv_color_hex(COLOR_TEXT), 0);

    int chars_per_page = 72;
    int start = s_transcribe_current_page * chars_per_page;
    int len = strlen(s_transcribe_text);
    if (start >= len) start = 0;

    char page_text[80] = {0};
    int copy_len = len - start;
    if (copy_len > chars_per_page) copy_len = chars_per_page;
    memcpy(page_text, s_transcribe_text + start, copy_len);
    page_text[copy_len] = '\0';

    lv_label_set_text(text, page_text);
    lv_obj_align(text, LV_ALIGN_TOP_LEFT, 0, 24);

    lv_obj_t *page = lv_label_create(s_content_panel);
    char page_str[32];
    snprintf(page_str, sizeof(page_str), "[%d/%d]",
             s_transcribe_current_page + 1, s_transcribe_pages);
    lv_label_set_text(page, page_str);
    lv_obj_set_style_text_color(page, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(page, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

// ── Rebuild UI (caller must hold LVGL lock) ────────────────────────────────

static void rebuild_ui(void) {
    // Update status/hint labels
    switch (s_state) {
        case STATE_PAIRING:
            lv_label_set_text(s_status_label, "Hermes Passport");
            lv_label_set_text(s_hint_label, "Waiting for BLE...");
            break;
        case STATE_CONNECTING:
            lv_label_set_text(s_status_label, "Connected");
            lv_label_set_text(s_hint_label, "Loading bots...");
            break;
        case STATE_BOT_LIST:
            lv_label_set_text(s_status_label, "Bots");
            lv_label_set_text(s_hint_label, "UP/DOWN:sel OK:enter");
            rebuild_bot_list_ui();
            break;
        case STATE_CHAT_INPUT:
            lv_label_set_text(s_status_label, s_bots[s_bot_selected].name);
            lv_label_set_text(s_hint_label, "UP:cont DN:stop OK:rec");
            rebuild_chat_ui();
            break;
        case STATE_CHAT_SCROLL:
            lv_label_set_text(s_status_label, s_bots[s_bot_selected].name);
            lv_label_set_text(s_hint_label, "UP/DN:scroll OK:exit");
            rebuild_chat_ui();
            break;
        case STATE_RECORDING:
            lv_label_set_text(s_status_label, s_bots[s_bot_selected].name);
            lv_label_set_text(s_hint_label, "Recording... OK:stop");
            break;
        case STATE_TRANSCRIBE:
            lv_label_set_text(s_status_label, s_bots[s_bot_selected].name);
            lv_label_set_text(s_hint_label, "UP:retry DN:page OK:send");
            rebuild_transcribe_ui();
            break;
        case STATE_ERROR:
            lv_label_set_text(s_status_label, "Error");
            lv_label_set_text(s_hint_label, "OK:retry");
            break;
    }
}

// ── State Machine ──────────────────────────────────────────────────────────

static void set_state(ui_state_t new_state) {
    ESP_LOGI(TAG, "State: %d -> %d", s_state, new_state);
    s_state = new_state;
    if (!bsp_lvgl_lock(500)) return;
    rebuild_ui();
    bsp_lvgl_unlock();
}

// ── BLE Callbacks ──────────────────────────────────────────────────────────

static void on_ble_connected(void) {
    ESP_LOGI(TAG, "BLE connected");
    set_state(STATE_CONNECTING);
}

static void on_ble_disconnected(void) {
    ESP_LOGW(TAG, "BLE disconnected");
    set_state(STATE_PAIRING);
}

static void on_ble_bots(const hermes_ble_bot_t *bots, int count) {
    ESP_LOGI(TAG, "Received %d bots", count);
    s_bot_count = count > MAX_BOTS ? MAX_BOTS : count;
    memcpy(s_bots, bots, s_bot_count * sizeof(hermes_ble_bot_t));
    s_bot_selected = 0;
    set_state(STATE_BOT_LIST);
}

static void on_ble_stream_chunk(const char *request_id, const char *content) {
    ESP_LOGI(TAG, "Stream chunk: %s", content);
    add_message(content, false, false);
    // Rebuild UI if in chat mode
    if (s_state == STATE_CHAT_INPUT || s_state == STATE_CHAT_SCROLL) {
        if (bsp_lvgl_lock(500)) {
            rebuild_chat_ui();
            bsp_lvgl_unlock();
        }
    }
}

static void on_ble_stream_end(const char *request_id) {
    ESP_LOGI(TAG, "Stream end");
}

static void on_ble_stt_result(const char *request_id, const char *text) {
    ESP_LOGI(TAG, "STT result: %s", text);
    strncpy(s_transcribe_text, text, sizeof(s_transcribe_text) - 1);
    int text_len = strlen(text);
    int chars_per_page = 72;
    s_transcribe_pages = (text_len + chars_per_page - 1) / chars_per_page;
    if (s_transcribe_pages < 1) s_transcribe_pages = 1;
    s_transcribe_current_page = 0;
    set_state(STATE_TRANSCRIBE);
}

static void on_ble_stt_error(const char *request_id, const char *error) {
    ESP_LOGE(TAG, "STT error: %s", error);
    add_message("STT failed", false, true);
    set_state(STATE_CHAT_INPUT);
}

// ── Messages ───────────────────────────────────────────────────────────────

static void add_message(const char *text, bool is_user, bool is_system) {
    int idx = (s_msg_head + 1) % MAX_MESSAGES;
    chat_message_t *msg = &s_messages[idx];
    strncpy(msg->text, text, MAX_MSG_TEXT - 1);
    msg->text[MAX_MSG_TEXT - 1] = '\0';
    msg->is_user = is_user;
    msg->is_system = is_system;
    s_msg_head = idx;
    if (s_msg_count < MAX_MESSAGES) s_msg_count++;
}

// ── Audio Task ─────────────────────────────────────────────────────────────

static void audio_task_func(void *arg) {
    (void)arg;

    int16_t *pcm_buf = malloc(OPUS_FRAME_SAMPLES * sizeof(int16_t));
    if (!pcm_buf) {
        ESP_LOGE(TAG, "Audio buffer alloc failed");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        if (!s_recording) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (bsp_audio_set_format(AUDIO_SAMPLE_RATE, 16, 1) != ESP_OK) {
            ESP_LOGE(TAG, "Audio format failed");
            s_recording = false;
            continue;
        }

        s_audio_sequence = 0;
        ESP_LOGI(TAG, "Recording started");

        while (s_recording && !s_audio_stop) {
            if (bsp_audio_read(pcm_buf, OPUS_FRAME_SAMPLES * sizeof(int16_t)) != ESP_OK) {
                ESP_LOGE(TAG, "Audio read failed");
                break;
            }
            // TODO: Opus encode + send via BLE
            s_audio_sequence++;
        }

        hermes_ble_audio_end();
        s_recording = false;
        s_audio_stop = false;
        ESP_LOGI(TAG, "Recording ended, %d frames", s_audio_sequence);
    }

    free(pcm_buf);
}

// ── Key Handler ────────────────────────────────────────────────────────────
// NOTE: Called from on_key() which already holds LVGL lock.
//       Do NOT call bsp_lvgl_lock() here — use rebuild_* directly.

void hermes_bridge_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    switch (s_state) {
        case STATE_PAIRING:
            break;

        case STATE_BOT_LIST:
            if (btn == BSP_BTN_UP && ev == BSP_BTN_CLICK) {
                if (s_bot_selected > 0) {
                    s_bot_selected--;
                    rebuild_bot_list_ui();
                }
            } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_CLICK) {
                if (s_bot_selected < s_bot_count - 1) {
                    s_bot_selected++;
                    rebuild_bot_list_ui();
                }
            } else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
                if (s_bot_count > 0) {
                    s_msg_count = 0;
                    s_msg_head = 0;
                    hermes_ble_open_bot(s_bots[s_bot_selected].id);
                    // Update state and UI inline (lock already held)
                    s_state = STATE_CHAT_INPUT;
                    rebuild_ui();
                }
            }
            break;

        case STATE_CHAT_INPUT:
            if (btn == BSP_BTN_UP && ev == BSP_BTN_CLICK) {
                hermes_ble_send_quick(s_bots[s_bot_selected].id, 1);
                add_message("continue", true, false);
                rebuild_chat_ui();
            } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_CLICK) {
                hermes_ble_send_quick(s_bots[s_bot_selected].id, 2);
                add_message("/stop", true, false);
                rebuild_chat_ui();
            } else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
                s_recording = true;
                s_audio_stop = false;
                hermes_ble_audio_start(s_bots[s_bot_selected].id);
                s_state = STATE_RECORDING;
                rebuild_ui();
            } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_LONG) {
                // Back to bot list
                s_state = STATE_BOT_LIST;
                rebuild_ui();
            } else if (btn == BSP_BTN_UP && ev == BSP_BTN_LONG) {
                s_state = STATE_CHAT_SCROLL;
                rebuild_ui();
            }
            break;

        case STATE_CHAT_SCROLL:
            if (btn == BSP_BTN_UP && ev == BSP_BTN_CLICK) {
                // TODO: scroll up
            } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_CLICK) {
                // TODO: scroll down
            } else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
                s_state = STATE_CHAT_INPUT;
                rebuild_ui();
            } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_LONG) {
                s_state = STATE_BOT_LIST;
                rebuild_ui();
            }
            break;

        case STATE_RECORDING:
            if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
                s_audio_stop = true;
            }
            break;

        case STATE_TRANSCRIBE:
            if (btn == BSP_BTN_UP && ev == BSP_BTN_CLICK) {
                s_state = STATE_CHAT_INPUT;
                rebuild_ui();
            } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_CLICK) {
                s_transcribe_current_page++;
                if (s_transcribe_current_page >= s_transcribe_pages)
                    s_transcribe_current_page = 0;
                rebuild_transcribe_ui();
            } else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
                hermes_ble_send_text(s_bots[s_bot_selected].id, s_transcribe_text);
                add_message(s_transcribe_text, true, false);
                s_state = STATE_CHAT_INPUT;
                rebuild_ui();
            } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_LONG) {
                s_state = STATE_CHAT_INPUT;
                rebuild_ui();
            }
            break;

        case STATE_ERROR:
            if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
                s_state = STATE_PAIRING;
                rebuild_ui();
            }
            break;

        default:
            break;
    }
}

// ── Page Lifecycle ─────────────────────────────────────────────────────────

void hermes_bridge_enter(void) {
    ESP_LOGI(TAG, "Hermes Bridge entering (BLE mode)");

    s_scr = ui_pixel_screen_create("HERMES");

    s_status_label = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_16, 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, 8);

    s_hint_label = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_font(s_hint_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -8);

    s_state = STATE_PAIRING;
    s_bot_count = 0;
    s_bot_selected = 0;
    s_msg_count = 0;
    s_msg_head = 0;
    s_recording = false;

    rebuild_ui();
    lv_screen_load(s_scr);

    hermes_ble_set_callbacks(
        on_ble_bots, on_ble_stream_chunk, on_ble_stream_end,
        on_ble_stt_result, on_ble_stt_error, on_ble_connected, on_ble_disconnected
    );
    hermes_ble_init();
    hermes_ble_start();

    xTaskCreate(audio_task_func, "hermes_audio", AUDIO_TASK_STACK, NULL, 5, &s_audio_task);
}

void hermes_bridge_exit(void) {
    ESP_LOGI(TAG, "Hermes Bridge exiting");
    hermes_ble_stop();
    s_recording = false;
    s_audio_stop = true;
    if (s_audio_task) { vTaskDelete(s_audio_task); s_audio_task = NULL; }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_status_label = s_content_panel = s_hint_label = NULL;
    }
}
