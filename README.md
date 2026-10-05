<p align="right">
  <strong>English</strong> · <a href="README.zh_CN.md">简体中文</a>
</p>

# Hermes Passport

Hermes client firmware for the FoloToy AI Passport (ESP32-C3): the device connects to a Hermes Desktop companion plugin over Bluetooth LE and provides multi-bot chat, voice input, and streaming replies.

## Features

- **Multi-bot selection**: pick from the bot list served by the companion (individual bots or group chats, whatever the companion registers)
- **Voice input**: record → PCM16 streamed over BLE → companion runs STT → transcribed text shown for confirmation → send
- **Streaming output**: bot replies arrive over BLE and render live on screen
- **BLE pairing**: the companion connects to the device's GATT service; pairing info is stored in NVS for auto-reconnect
- **Dynamic bot list**: fetched from the companion at connect time, never hardcoded

## Architecture

```
AI Passport (ESP32-C3)             Hermes Desktop Plugin (Companion)
        │                                   │
   LVGL UI + buttons                  BLE central
   NimBLE GATT server                 Gateway JSON-RPC client
   ES8311 audio (PCM16 capture)       STT bridge + bot registry
        │                                   │
        └──────── Bluetooth LE ─────────────┤
                                            │
                                     Hermes Gateway
                                     (profiles)
```

- Device = BLE peripheral / GATT server, advertises as `Hermes-Passport`
- Companion = BLE central, bridges the device to Hermes Gateway profiles

## Hardware

- **MCU**: ESP32-C3, 8 MB Flash, no PSRAM
- **Display**: 240×320 ST7789
- **Buttons**: 3 (UP/DOWN/OK, ADC resistor ladder)
- **Audio**: ES8311 (I2S full duplex)
- **WiFi**: 2.4 GHz (not used in BLE mode)

## Buttons

| Button | Short press | Long press |
|--------|-------------|------------|
| UP | Send "continue" | Enter scroll mode |
| DOWN | Send "/stop" | Back to bot list |
| OK | Start/stop recording | Confirm selection |

## BLE Protocol

Service `48450001-51c4-499d-a186-4621a4938301` (advertised as `Hermes-Passport`):

| Characteristic | UUID | Direction | Purpose |
|---|---|---|---|
| RX (write) | `…8302` | Companion → device | Control messages: bot list, stream chunks, STT result |
| TX (notify) | `…8303` | Device → Companion | Control messages: send text, quick actions, open bot |
| VOICE (write/notify) | `…8304` | Device → Companion | PCM16 audio frames for STT |

Message format: `version:u8 | type:u8 | payload`.
VOICE frames: `kind:u8 | token:u32LE | sequence:u16LE | payload` — kind 1=start, 2=data, 3=end, 4=cancel.

## Development

### Prerequisites

ESP-IDF v5.5.3 — installation details in [docs/ESP-IDF-INSTALL.md](docs/ESP-IDF-INSTALL.md).

### Build

```bash
cd hermes-passport
idf.py set-target esp32c3
idf.py build
idf.py -p COM3 flash monitor   # replace COM3 with the actual port
```

### Validation

```bash
./tools/validate.sh --static    # repository checks + host tests
./tools/validate.sh --firmware  # ESP-IDF build + merged-image verification
./tools/validate.sh             # complete gate
```

## Project Structure

```
hermes-passport/
├── main/
│   ├── main.c               ← demo menu entry
│   ├── hermes_bridge.c/.h   ← Hermes page: LVGL UI + state machine
│   ├── hermes_ble.c/.h      ← NimBLE GATT server + protocol
│   └── ui_pixel.c/.h        ← pixel UI helpers
├── components/bsp/          ← board support (display, audio, buttons, ...)
├── docs/                    ← development, hardware, and contribution docs
├── tools/                   ← validation scripts
└── tests/                   ← host tests
```

## Companion Plugin

The companion runs on the desktop as a Hermes plugin:

- Frontend: `~/.hermes/desktop-plugins/hermes-bridge/plugin.js`
- Backend: `~/.hermes/plugins/hermes-bridge/dashboard/plugin_api.py`

## License

MIT License
