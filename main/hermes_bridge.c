// main/hermes_bridge.c — Hermes Bridge client for AI Passport
// Connects to Hermes Desktop plugin via WiFi, provides bot selection + voice chat
//
// Architecture:
//   ESP32 --WiFi HTTP/WS--> Desktop Plugin (:9527) --JSON-RPC--> Hermes Gateway
//
// UI States:
//   1. Connecting     - WiFi + server connection
//   2. Bot List       - Select bot/group to chat with
//   3. Chat           - Text + voice interaction with selected bot
//   4. Recording      - Audio recording, streaming PCM to backend
//   5. Error          - Connection lost, retry

#include "hermes_bridge.h"
#include "demo.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "ui_pixel.h"

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "hermes_bridge";

// ── Configuration ──────────────────────────────────────────────────────────
#define SERVER_PORT         9527
#define MAX_BOTS            8
#define MAX_GROUPS          4
#define MAX_BOT_NAME        32
#define MAX_BOT_DESC        64
#define MAX_MSG_TEXT        256
#define MAX_MESSAGES        20
#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_CHUNK_SAMPLES 512   // 1KB per chunk
#define AUDIO_TASK_STACK    4096
#define HTTP_TASK_STACK     4096
#define WS_BUFFER_SIZE      4096

// ── UI Colors ──────────────────────────────────────────────────────────────
#define COLOR_BG            0x000000
#define COLOR_USER_MSG      0x1a73e8
#define COLOR_BOT_MSG       0x2d2d2d
#define COLOR_TEXT          0xffffff
#define COLOR_TEXT_DIM      0x9e9e9e
#define COLOR_HIGHLIGHT     0x3949ab
#define COLOR_ACCENT        0x4caf50
#define COLOR_ERROR         0xf44336
#define COLOR_ONLINE        0x4caf50
#define COLOR_OFFLINE       0x9e9e9e

// ── Data Structures ────────────────────────────────────────────────────────

typedef struct {
    char id[MAX_BOT_NAME];
    char name[MAX_BOT_NAME];
    char description[MAX_BOT_DESC];
    char icon[MAX_BOT_NAME];
    char profile[MAX_BOT_NAME];
    bool is_group;
    bool online;
} bot_entry_t;

typedef struct {
    char text[MAX_MSG_TEXT];
    bool is_user;
    bool is_system;
    uint32_t timestamp;
} chat_message_t;

typedef enum {
    STATE_CONNECTING = 0,
    STATE_BOT_LIST,
    STATE_CHAT_INPUT,     // Chat - input mode (auto-scroll)
    STATE_CHAT_SCROLL,    // Chat - scroll mode (manual browse)
    STATE_RECORDING,      // Recording audio
    STATE_STT_PROCESSING, // Waiting for STT result
    STATE_ERROR,
} ui_state_t;

// ── Global State ───────────────────────────────────────────────────────────

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_content_panel = NULL;
static lv_obj_t *s_hint_label = NULL;

static ui_state_t s_state = STATE_CONNECTING;
static TaskHandle_t s_http_task = NULL;
static TaskHandle_t s_audio_task = NULL;

// Bot registry
static bot_entry_t s_bots[MAX_BOTS];
static int s_bot_count = 0;
static int s_bot_selected = 0;

// Chat messages (ring buffer)
static chat_message_t s_messages[MAX_MESSAGES];
static int s_msg_count = 0;
static int s_msg_head = 0;  // newest message index

// Server connection
static char s_server_ip[16] = {0};
static uint16_t s_server_port = SERVER_PORT;
static bool s_connected = false;

// Audio recording
static volatile bool s_recording = false;
static volatile bool s_audio_stop = false;
static char s_request_id[37] = {0};  // UUID string

// ── Forward Declarations ───────────────────────────────────────────────────

static void set_state(ui_state_t new_state);
static void rebuild_ui(void);
static void http_task_func(void *arg);
static void audio_task_func(void *arg);
static bool fetch_bot_list(void);
static bool send_chat_message(const char *bot_id, const char *text);
static void add_message(const char *text, bool is_user, bool is_system);
static void generate_uuid(char *buf, size_t len);

// ── UI Helpers ─────────────────────────────────────────────────────────────

static void set_hint(const char *text) {
    if (!s_hint_label) return;
    if (!bsp_lvgl_lock(500)) return;
    lv_label_set_text(s_hint_label, text);
    bsp_lvgl_unlock();
}

static void set_status(const char *text) {
    if (!s_status_label) return;
    if (!bsp_lvgl_lock(500)) return;
    lv_label_set_text(s_status_label, text);
    bsp_lvgl_unlock();
}

// ── Bot List UI ────────────────────────────────────────────────────────────

static void rebuild_bot_list_ui(void) {
    if (!bsp_lvgl_lock(500)) return;

    if (s_content_panel) {
        lv_obj_delete(s_content_panel);
        s_content_panel = NULL;
    }

    s_content_panel = lv_obj_create(s_scr);
    lv_obj_set_size(s_content_panel, 204, 200);
    lv_obj_align(s_content_panel, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_color(s_content_panel, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_border_width(s_content_panel, 0, 0);
    lv_obj_set_style_pad_all(s_content_panel, 4, 0);

    if (s_bot_count == 0) {
        lv_obj_t *empty = lv_label_create(s_content_panel);
        lv_label_set_text(empty, "No bots available");
        lv_obj_set_style_text_color(empty, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_center(empty);
    } else {
        for (int i = 0; i < s_bot_count; i++) {
            lv_obj_t *row = lv_obj_create(s_content_panel);
            lv_obj_set_size(row, 196, 40);
            lv_obj_set_style_bg_color(row,
                lv_color_hex(i == s_bot_selected ? COLOR_HIGHLIGHT : COLOR_BG), 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(COLOR_TEXT_DIM), 0);
            lv_obj_set_style_radius(row, 4, 0);
            lv_obj_set_pos(row, 0, i * 44);

            // Bot name
            lv_obj_t *name = lv_label_create(row);
            char label[MAX_BOT_NAME + 4];
            snprintf(label, sizeof(label), "%s %s",
                     s_bots[i].is_group ? "[G]" : "[B]",
                     s_bots[i].name);
            lv_label_set_text(name, label);
            lv_obj_set_style_text_color(name, lv_color_hex(COLOR_TEXT), 0);
            lv_obj_align(name, LV_ALIGN_LEFT_MID, 4, -4);

            // Status dot
            lv_obj_t *dot = lv_obj_create(row);
            lv_obj_set_size(dot, 8, 8);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot,
                lv_color_hex(s_bots[i].online ? COLOR_ONLINE : COLOR_OFFLINE), 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_align(dot, LV_ALIGN_RIGHT_MID, -4, -4);

            // Description
            lv_obj_t *desc = lv_label_create(row);
            lv_label_set_text(desc, s_bots[i].description);
            lv_obj_set_style_text_color(desc, lv_color_hex(COLOR_TEXT_DIM), 0);
            lv_obj_set_style_text_font(desc, &lv_font_montserrat_12, 0);
            lv_obj_align(desc, LV_ALIGN_LEFT_MID, 4, 8);
        }
    }

    bsp_lvgl_unlock();
}

// ── Chat UI ────────────────────────────────────────────────────────────────

static void rebuild_chat_ui(void) {
    if (!bsp_lvgl_lock(500)) return;

    if (s_content_panel) {
        lv_obj_delete(s_content_panel);
        s_content_panel = NULL;
    }

    s_content_panel = lv_obj_create(s_scr);
    lv_obj_set_size(s_content_panel, 204, 200);
    lv_obj_align(s_content_panel, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_color(s_content_panel, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_border_width(s_content_panel, 0, 0);
    lv_obj_set_style_pad_all(s_content_panel, 4, 0);
    lv_obj_set_flex_flow(s_content_panel, LV_FLEX_FLOW_COLUMN);

    // Display messages (newest at bottom)
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
            lv_obj_set_style_margin_left(bubble, 20);
            lv_obj_set_flex_align(bubble, LV_FLEX_ALIGN_END, 0, 0);
        } else {
            lv_obj_set_style_bg_color(bubble, lv_color_hex(COLOR_BOT_MSG), 0);
            lv_obj_set_style_border_width(bubble, 0, 0);
            lv_obj_set_style_margin_right(bubble, 20);
        }

        lv_obj_t *text = lv_label_create(bubble);
        lv_label_set_text(text, msg->text);
        lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(text, 178);
        lv_obj_set_style_text_color(text, lv_color_hex(
            msg->is_system ? COLOR_TEXT_DIM : COLOR_TEXT), 0);
    }

    bsp_lvgl_unlock();
}

static void rebuild_ui(void) {
    switch (s_state) {
        case STATE_CONNECTING:
            set_status("Connecting...");
            set_hint("Please wait");
            break;
        case STATE_BOT_LIST:
            set_status("Bot List");
            set_hint("UP/DOWN:select OK:enter");
            rebuild_bot_list_ui();
            break;
        case STATE_CHAT_INPUT:
        case STATE_CHAT_SCROLL:
        case STATE_RECORDING:
        case STATE_STT_PROCESSING:
            set_status(s_bots[s_bot_selected].name);
            rebuild_chat_ui();
            if (s_state == STATE_RECORDING) {
                set_hint("Recording... OK:stop");
            } else if (s_state == STATE_STT_PROCESSING) {
                set_hint("Recognizing...");
            } else if (s_state == STATE_CHAT_SCROLL) {
                set_hint("UP/DOWN:scroll OK:exit");
            } else {
                set_hint("UP:cont DOWN:stop OK:record");
            }
            break;
        case STATE_ERROR:
            set_status("Error");
            set_hint("OK:retry DOWN:back");
            break;
    }
}

// ── Messages ───────────────────────────────────────────────────────────────

static void add_message(const char *text, bool is_user, bool is_system) {
    int idx = (s_msg_head + 1) % MAX_MESSAGES;
    chat_message_t *msg = &s_messages[idx];
    strncpy(msg->text, text, MAX_MSG_TEXT - 1);
    msg->text[MAX_MSG_TEXT - 1] = '\0';
    msg->is_user = is_user;
    msg->is_system = is_system;
    msg->timestamp = 0;  // TODO: get timestamp
    s_msg_head = idx;
    if (s_msg_count < MAX_MESSAGES) s_msg_count++;
}

// ── UUID Generator ─────────────────────────────────────────────────────────

static void generate_uuid(char *buf, size_t len) {
    // Simple UUID v4 (not cryptographically secure, good enough for request IDs)
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            buf[i] = '-';
        } else {
            buf[i] = hex[rand() % 16];
        }
    }
    buf[36] = '\0';
}

// ── HTTP Client ────────────────────────────────────────────────────────────

static esp_err_t http_get_json(const char *path, char *buf, size_t buf_len) {
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d%s", s_server_ip, s_server_port, path);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int read = esp_http_client_read(client, buf, buf_len - 1);
    if (read > 0) buf[read] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return (read > 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t http_post_json(const char *path, const char *body, char *resp, size_t resp_len) {
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d%s", s_server_ip, s_server_port, path);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_err_t err = esp_http_client_open(client, strlen(body));
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_write(client, body, strlen(body));
    int content_length = esp_http_client_fetch_headers(client);
    int read = esp_http_client_read(client, resp, resp_len - 1);
    if (read > 0) resp[read] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return (read > 0) ? ESP_OK : ESP_FAIL;
}

// ── Bot List Fetch ─────────────────────────────────────────────────────────

static bool fetch_bot_list(void) {
    char buf[2048];
    if (http_get_json("/api/bots", buf, sizeof(buf)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to fetch bot list");
        return false;
    }

    // Parse JSON manually (no cJSON dependency to save memory)
    // Simple parser: look for "id" and "name" fields
    s_bot_count = 0;

    char *p = buf;
    while (s_bot_count < MAX_BOTS && (p = strstr(p, "\"id\"")) != NULL) {
        bot_entry_t *bot = &s_bots[s_bot_count];

        // Extract id
        p = strchr(p, ':');
        if (!p) break;
        p = strchr(p, '"');
        if (!p) break;
        p++;
        char *end = strchr(p, '"');
        if (!end) break;
        size_t len = end - p;
        if (len >= MAX_BOT_NAME) len = MAX_BOT_NAME - 1;
        strncpy(bot->id, p, len);
        bot->id[len] = '\0';

        // Extract name
        p = strstr(end, "\"name\"");
        if (!p) break;
        p = strchr(p, ':');
        if (!p) break;
        p = strchr(p, '"');
        if (!p) break;
        p++;
        end = strchr(p, '"');
        if (!end) break;
        len = end - p;
        if (len >= MAX_BOT_NAME) len = MAX_BOT_NAME - 1;
        strncpy(bot->name, p, len);
        bot->name[len] = '\0';

        // Extract description
        p = strstr(end, "\"description\"");
        if (p) {
            p = strchr(p, ':');
            if (p) {
                p = strchr(p, '"');
                if (p) {
                    p++;
                    end = strchr(p, '"');
                    if (end) {
                        len = end - p;
                        if (len >= MAX_BOT_DESC) len = MAX_BOT_DESC - 1;
                        strncpy(bot->description, p, len);
                        bot->description[len] = '\0';
                    }
                }
            }
        }

        // Check type
        p = strstr(p, "\"type\"");
        bot->is_group = (p && strstr(p, "\"group\""));

        // Check status
        p = strstr(p, "\"status\"");
        bot->online = (p && strstr(p, "\"online\""));

        s_bot_count++;
    }

    ESP_LOGI(TAG, "Fetched %d bots", s_bot_count);
    return s_bot_count > 0;
}

// ── Send Chat Message ──────────────────────────────────────────────────────

static bool send_chat_message(const char *bot_id, const char *text) {
    char body[512];
    char resp[256];
    generate_uuid(s_request_id, sizeof(s_request_id));

    snprintf(body, sizeof(body),
        "{\"bot\":\"%s\",\"content\":\"%s\",\"request_id\":\"%s\"}",
        bot_id, text, s_request_id);

    if (http_post_json("/api/chat", body, resp, sizeof(resp)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send message");
        return false;
    }

    add_message(text, true, false);
    return true;
}

// ── HTTP Task ──────────────────────────────────────────────────────────────

static void http_task_func(void *arg) {
    (void)arg;

    // Wait for server IP to be configured
    while (s_server_ip[0] == '\0') {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // Try to connect and fetch bot list
    for (int retry = 0; retry < 10; retry++) {
        if (fetch_bot_list()) {
            s_connected = true;
            set_state(STATE_BOT_LIST);
            break;
        }
        ESP_LOGW(TAG, "Connection attempt %d failed, retrying...", retry + 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    if (!s_connected) {
        set_state(STATE_ERROR);
    }

    // Keep running for periodic updates
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (s_connected) {
            fetch_bot_list();
        }
    }
}

// ── Audio Task ─────────────────────────────────────────────────────────────

static void audio_task_func(void *arg) {
    (void)arg;

    int16_t *chunk = malloc(AUDIO_CHUNK_SAMPLES * sizeof(int16_t));
    if (!chunk) {
        ESP_LOGE(TAG, "Audio chunk buffer alloc failed");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        if (!s_recording) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Record and stream PCM chunks
        if (bsp_audio_set_format(AUDIO_SAMPLE_RATE, 16, 1) != ESP_OK) {
            ESP_LOGE(TAG, "Audio format failed");
            s_recording = false;
            continue;
        }

        ESP_LOGI(TAG, "Recording started, streaming to server");

        // Send audio_start
        char body[256];
        snprintf(body, sizeof(body),
            "{\"action\":\"audio_start\",\"bot\":\"%s\",\"format\":\"pcm_16k_16bit_mono\",\"request_id\":\"%s\"}",
            s_bots[s_bot_selected].id, s_request_id);
        http_post_json("/api/audio/start", body, body, sizeof(body));

        while (s_recording && !s_audio_stop) {
            // Read one chunk
            if (bsp_audio_read(chunk, AUDIO_CHUNK_SAMPLES * sizeof(int16_t)) != ESP_OK) {
                break;
            }

            // Send chunk via HTTP POST
            // TODO: Switch to WebSocket binary frame for efficiency
            char url[128];
            snprintf(url, sizeof(url), "http://%s:%d/api/audio/chunk",
                     s_server_ip, s_server_port);

            esp_http_client_config_t cfg = {
                .url = url,
                .method = HTTP_METHOD_POST,
                .timeout_ms = 2000,
            };
            esp_http_client_handle_t client = esp_http_client_init(&cfg);
            if (client) {
                esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
                esp_http_client_set_header(client, "X-Request-ID", s_request_id);
                esp_err_t err = esp_http_client_open(client, AUDIO_CHUNK_SAMPLES * sizeof(int16_t));
                if (err == ESP_OK) {
                    esp_http_client_write(client, (const char *)chunk,
                                         AUDIO_CHUNK_SAMPLES * sizeof(int16_t));
                    esp_http_client_close(client);
                }
                esp_http_client_cleanup(client);
            }
        }

        // Send audio_end
        snprintf(body, sizeof(body), "{\"request_id\":\"%s\"}", s_request_id);
        http_post_json("/api/audio/end", body, body, sizeof(body));

        ESP_LOGI(TAG, "Recording ended");
        s_recording = false;
        s_audio_stop = false;

        set_state(STATE_STT_PROCESSING);

        // STT result will come back as a chat message
        // The backend will automatically send it to the bot
    }

    free(chunk);
}

// ── State Machine ──────────────────────────────────────────────────────────

static void set_state(ui_state_t new_state) {
    ESP_LOGI(TAG, "State: %d -> %d", s_state, new_state);
    s_state = new_state;

    if (!bsp_lvgl_lock(500)) return;
    rebuild_ui();
    bsp_lvgl_unlock();
}

// ── Key Handler ────────────────────────────────────────────────────────────

void hermes_bridge_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    switch (s_state) {
        case STATE_CONNECTING:
            // No action during connection
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
                    add_message("Connected", false, true);
                    set_state(STATE_CHAT_INPUT);
                }
            }
            break;

        case STATE_CHAT_INPUT:
            if (btn == BSP_BTN_UP && ev == BSP_BTN_CLICK) {
                // Send "继续"
                send_chat_message(s_bots[s_bot_selected].id, "继续");
                rebuild_chat_ui();
            } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_CLICK) {
                // Send "/stop"
                send_chat_message(s_bots[s_bot_selected].id, "/stop");
                rebuild_chat_ui();
            } else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
                // Start recording
                s_recording = true;
                s_audio_stop = false;
                generate_uuid(s_request_id, sizeof(s_request_id));
                set_state(STATE_RECORDING);
            } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
                // Return to bot list
                set_state(STATE_BOT_LIST);
            } else if (btn == BSP_BTN_UP && ev == BSP_BTN_LONG) {
                // Enter scroll mode
                set_state(STATE_CHAT_SCROLL);
            }
            break;

        case STATE_CHAT_SCROLL:
            if (btn == BSP_BTN_UP && ev == BSP_BTN_CLICK) {
                // Scroll up (TODO: implement scroll offset)
            } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_CLICK) {
                // Scroll down
            } else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
                // Exit scroll mode
                set_state(STATE_CHAT_INPUT);
            }
            break;

        case STATE_RECORDING:
            if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
                // Stop recording
                s_audio_stop = true;
            }
            break;

        case STATE_STT_PROCESSING:
            // No action while processing
            break;

        case STATE_ERROR:
            if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
                // Retry connection
                s_connected = false;
                set_state(STATE_CONNECTING);
                if (s_http_task) {
                    vTaskDelete(s_http_task);
                    s_http_task = NULL;
                }
                xTaskCreate(http_task_func, "hermes_http", HTTP_TASK_STACK, NULL, 4, &s_http_task);
            } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_CLICK) {
                // Return to main menu
                hermes_bridge_exit();
            }
            break;
    }
}

// ── Page Lifecycle ─────────────────────────────────────────────────────────

void hermes_bridge_enter(void) {
    ESP_LOGI(TAG, "Hermes Bridge entering");

    // Create screen
    s_scr = ui_pixel_screen_create("HERMES");

    // Status bar
    s_status_label = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_16, 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, 8);

    // Content panel (will be populated by state)
    s_content_panel = NULL;

    // Hint bar at bottom
    s_hint_label = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_font(s_hint_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -8);

    // Initialize state
    s_state = STATE_CONNECTING;
    s_bot_count = 0;
    s_bot_selected = 0;
    s_msg_count = 0;
    s_msg_head = 0;
    s_connected = false;
    s_recording = false;

    rebuild_ui();
    lv_screen_load(s_scr);

    // Start HTTP task
    xTaskCreate(http_task_func, "hermes_http", HTTP_TASK_STACK, NULL, 4, &s_http_task);

    // Start audio task
    xTaskCreate(audio_task_func, "hermes_audio", AUDIO_TASK_STACK, NULL, 5, &s_audio_task);
}

void hermes_bridge_exit(void) {
    ESP_LOGI(TAG, "Hermes Bridge exiting");

    // Stop tasks
    s_recording = false;
    s_audio_stop = true;

    if (s_http_task) {
        vTaskDelete(s_http_task);
        s_http_task = NULL;
    }
    if (s_audio_task) {
        vTaskDelete(s_audio_task);
        s_audio_task = NULL;
    }

    // Delete UI
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_status_label = NULL;
        s_content_panel = NULL;
        s_hint_label = NULL;
    }
}
