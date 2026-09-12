# Hermes Passport

Hermes 客户端应用：ESP32-C3 AI Passport 通过 WiFi 连接 Hermes Desktop 插件后端，实现多 bot 聊天、语音交互、流式输出。

## 功能特性

- **多 Bot 选择**：🐻 思考熊 / 🐙 八爪鱼 / 🕷️ 织网蛛 / 💬 群聊
- **语音交互**：录音 → PCM 流式上传 → 后端 STT → 自动发送
- **流式输出**：WebSocket 实时接收 bot 回复，逐字显示
- **BLUFI 配网**：通过 ESP Config 手机 App 配置 WiFi
- **动态 Bot 列表**：从后端动态获取可用 bot

## 架构

```
AI Passport (ESP32-C3)          Hermes Desktop Plugin (:9527)
        │                                │
   LVGL UI + 按键                    HTTP Server
   WiFi + HTTP Client                WebSocket Server
   ES8311 音频                       Bot Registry
        │                                │
        └──── WiFi ────────────────────► Gateway
                                            │
                                     Hermes Gateway
                                     (各 Profile)
```

## 硬件

- **MCU**：ESP32-C3, 8MB Flash, 无 PSRAM
- **显示屏**：240×320 ST7789
- **按键**：3 个 (UP/DOWN/OK，ADC 电阻梯形)
- **音频**：ES8311 (I2S 全双工)
- **WiFi**：2.4GHz

## 按键定义

| 按键 | 短按 | 长按 |
|------|------|------|
| UP   | 发送 "继续" | 进入滚动模式 |
| DOWN | 发送 "/stop" | 返回 Bot 列表 |
| OK   | 开始/停止录音 | 确认选择 |

## 开发环境

### 安装 ESP-IDF

1. 下载 ESP-IDF v5.5.3 离线安装器：
   https://github.com/espressif/idf-installer/releases/download/offline-5.5.3/esp-idf-tools-setup-offline-5.5.3.exe

2. 运行安装器，安装到默认目录

3. 打开 "ESP-IDF 5.5 CMD"，进入项目目录

### 编译固件

```bash
cd hermes-passport
idf.py set-target esp32c3
idf.py build
idf.py -p COM3 flash monitor
```

## 项目结构

```
hermes-passport/
├── main/
│   ├── hermes_bridge.c      ← 主程序
│   ├── hermes_bridge.h
│   ├── hermes_wifi_prov.c   ← BLUFI 配网
│   ├── hermes_wifi_prov.h
│   ├── main.c               ← 菜单入口
│   └── CMakeLists.txt
├── components/bsp/          ← BSP 驱动
├── docs/
│   ├── TECHNICAL_DESIGN.md  ← 技术设计
│   ├── UI-UX.md             ← UI/UX 设计
│   └── ESP-IDF-INSTALL.md   ← 安装指南
└── README.md
```

## Desktop 插件

插件文件位于：
- 前端：`~/.hermes/desktop-plugins/hermes-bridge/plugin.js`
- 后端：`~/.hermes/plugins/hermes-bridge/dashboard/plugin_api.py`

### 插件 API

| 端点 | 方法 | 说明 |
|------|------|------|
| `/api/health` | GET | 健康检查 |
| `/api/bots` | GET | 获取 Bot 列表 |
| `/api/chat` | POST | 发送消息 |
| `/api/audio/start` | POST | 开始录音 |
| `/api/audio/chunk` | POST | 发送音频块 |
| `/api/audio/end` | POST | 结束录音 |

## 许可证

MIT License
