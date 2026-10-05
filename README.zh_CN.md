<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# Hermes Passport

Hermes 客户端固件，运行在 FoloToy AI Passport（ESP32-C3）上：设备通过蓝牙 LE 连接 Hermes Desktop 伴侣插件，实现多 bot 聊天、语音输入、流式输出。

## 功能特性

- **多 Bot 选择**：从伴侣插件提供的 bot 列表中选择（单 bot 或群聊，由伴侣插件注册）
- **语音输入**：录音 → PCM16 经 BLE 流式上传 → 伴侣插件跑 STT → 转写文本确认 → 发送
- **流式输出**：bot 回复经 BLE 实时送达，屏幕逐字渲染
- **BLE 配对**：伴侣插件连接设备的 GATT 服务，配对信息存 NVS，下次自动重连
- **动态 Bot 列表**：连接时从伴侣插件获取，不硬编码

## 架构

```
AI Passport (ESP32-C3)             Hermes Desktop 插件（Companion）
        │                                   │
   LVGL UI + 按键                    BLE central
   NimBLE GATT server                Gateway JSON-RPC 客户端
   ES8311 音频（PCM16 采集）          STT 桥接 + Bot 注册表
        │                                   │
        └──────── Bluetooth LE ─────────────┤
                                            │
                                     Hermes Gateway
                                     (各 Profile)
```

- 设备 = BLE 外设 / GATT server，广播名 `Hermes-Passport`
- Companion = BLE 中心设备，把设备桥接到 Hermes Gateway 各 Profile

## 硬件

- **MCU**：ESP32-C3，8MB Flash，无 PSRAM
- **显示屏**：240×320 ST7789
- **按键**：3 个（UP/DOWN/OK，ADC 电阻梯形）
- **音频**：ES8311（I2S 全双工）
- **WiFi**：2.4GHz（BLE 模式下不使用）

## 按键定义

| 按键 | 短按 | 长按 |
|------|------|------|
| UP | 发送"继续" | 进入滚动模式 |
| DOWN | 发送 "/stop" | 返回 Bot 列表 |
| OK | 开始/停止录音 | 确认选择 |

## BLE 协议

服务 `48450001-51c4-499d-a186-4621a4938301`（广播名 `Hermes-Passport`）：

| 特征 | UUID | 方向 | 用途 |
|------|------|------|------|
| RX (write) | `…8302` | Companion → 设备 | 控制消息：bot 列表、流式分片、STT 结果 |
| TX (notify) | `…8303` | 设备 → Companion | 控制消息：发送文本、快捷动作、打开 bot |
| VOICE (write/notify) | `…8304` | 设备 → Companion | PCM16 音频帧（用于 STT） |

消息格式：`version:u8 | type:u8 | payload`。
VOICE 帧：`kind:u8 | token:u32LE | sequence:u16LE | payload` — kind 1=开始、2=数据、3=结束、4=取消。

## 开发环境

### 前置条件

ESP-IDF v5.5.3 — 安装细节见 [docs/ESP-IDF-INSTALL.zh_CN.md](docs/ESP-IDF-INSTALL.zh_CN.md)。

### 编译

```bash
cd hermes-passport
idf.py set-target esp32c3
idf.py build
idf.py -p COM3 flash monitor   # COM3 替换为实际串口
```

### 验证

```bash
./tools/validate.sh --static    # 仓库检查 + 主机测试
./tools/validate.sh --firmware  # ESP-IDF 编译 + 合并镜像校验
./tools/validate.sh             # 完整门禁
```

## 项目结构

```
hermes-passport/
├── main/
│   ├── main.c               ← demo 菜单入口
│   ├── hermes_bridge.c/.h   ← Hermes 页面：LVGL UI + 状态机
│   ├── hermes_ble.c/.h      ← NimBLE GATT server + 协议
│   └── ui_pixel.c/.h        ← 像素 UI 辅助
├── components/bsp/          ← 板级驱动（显示、音频、按键……）
├── docs/                    ← 开发、硬件、贡献文档
├── tools/                   ← 验证脚本
└── tests/                   ← 主机测试
```

## Companion 插件

Companion 作为 Hermes 插件运行在桌面端：

- 前端：`~/.hermes/desktop-plugins/hermes-bridge/plugin.js`
- 后端：`~/.hermes/plugins/hermes-bridge/dashboard/plugin_api.py`

## 许可证

MIT License
