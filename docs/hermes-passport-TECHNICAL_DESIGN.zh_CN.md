# hermes-passport 技术设计

**[简体中文](hermes-passport-TECHNICAL_DESIGN.zh_CN.md)** | [English](hermes-passport-TECHNICAL_DESIGN.md)

与 [hermes-passport-UI-UX.zh_CN.md](hermes-passport-UI-UX.zh_CN.md) 配套。本文覆盖架构、契约、内存预算与分阶段实施计划。

参考输入：`ai-passport-hermes`（产品定义 + 硬件经验）与 `hermes-gadget-sdk`（设备认证 + 协议纪律）。
详见 `../../docs/ai-passport-hermes-review.md` 与 `../../docs/hermes-gadget-sdk-review.md`。

---

## 1. 架构

```
┌────────────────────────────┐     BLE GATT      ┌──────────────────────────┐
│  AI Passport (ESP32-C3)    │◀─────────────────▶│  伴侣插件                │
│                            │  Nordic UART +    │  (Hermes Desktop 插件)   │
│  main/  应用逻辑、UI        │  换行 JSON +      │                          │
│  components/bsp/  驱动     │  二进制语音        │  plugin.js  ─ UI 面板    │
│                            │                   │  plugin_api.py ─ BLE+RPC │
└────────────────────────────┘                   └────────────┬─────────────┘
                                                              │ host.request()
                                                              │ host.onEvent()
                                                 ┌────────────▼─────────────┐
                                                 │  Hermes Gateway          │
                                                 │  STT · agent 回合 · TTS  │
                                                 │  profiles · sessions     │
                                                 └──────────────────────────┘
```

**插件是与 Hermes 的唯一接触面。** 固件永远不知道 Gateway 内部结构。这与 `ai-passport-hermes`
的做法（读 `state.db`、注入按键、改 Hermes 源码）**刻意相反** —— 见 §7。

---

## 2. 组件划分

| 组件 | 位置 | 职责 | 可脱离设备测试 |
|---|---|---|---|
| `text_layout` | `main/text_layout.c` | 中文感知断行、整字符量宽、避头尾 | ✅ host test |
| `summary_extract` | `main/summary_extract.c` | Markdown → 纯文本摘要、抽取 SPK 块 | ✅ host test |
| `history_ring` | `main/history_ring.c` | 10 槽环形、去重、选页 | ✅ host test |
| `buddy_state` | `main/buddy_state.c` | UI 状态机（纯逻辑，不碰 LVGL） | ✅ host test |
| `adpcm` | `main/adpcm.c` | IMA-ADPCM 编码 + 溢出时重置解码状态 | ✅ host test |
| `nvs_settings` | `main/nvs_settings.c` | 设置 schema、加载/应用/恢复默认 | ✅ host test |
| `ble_protocol` | `main/hermes_ble.c` | GATT 帧、MTU 分片、序号 | shims |
| UI 渲染 | `main/hermes_bridge.c` | LVGL 控件，按状态 | ❌ 仅设备 |
| BSP | `components/bsp/` | 显示 / 音频 / 按键 / 电池 / I2C | ❌ 仅硬件 |

按 `AGENTS.md`：状态机、协议、时序与布局计算必须独立于 ESP-IDF/LVGL 并用 host tests 覆盖。
上面的划分正是为此。

---

## 3. 契约

### 3.1 BLE 帧（设备 ⇄ 插件）

控制帧是 NUS TX 特征上的换行分隔 JSON。二进制语音走 RX 特征，带 4 字节头。

```
二进制语音帧：  [kind u8][stream u8][seq u16 LE][payload ...]
  kind: 0x01 START | 0x02 DATA | 0x03 END | 0x04 CANCEL
  seq : 每个流独立，65535 回绕
```

控制消息（设备 → 插件）：

| `cmd` | 载荷 | 含义 |
|---|---|---|
| `hello` | `proto`, `device_id`, `name`, `fw` | 握手 |
| `key` | `k` (up/down/ok), `ev` (click/long) | 按键事件 |
| `setting` | `key`, `value` | 设备拨动一个镜像开关 |
| `decision` | `prompt_id`, `verdict` (confirm/deny) | 回答审批 |
| `ack` | `id`, `ok` | 送达回执 |

控制消息（插件 → 设备）：

| `type` | 载荷 | 含义 |
|---|---|---|
| `welcome` | `session`, `paired`, `heartbeat_s` | 握手完成 |
| `bots` | `[{id,name,status}]` | 动态 bot 列表（永不硬编码） |
| `reply.delta` | `request_id`, `text` | 流式文本 |
| `reply` | `request_id`, `text`, `summary` | 最终回复 + 已抽取的摘要 |
| `status` | `text` | 实时状态短语（"Checking the forecast"） |
| `decision` | `prompt_id`, `title`, `detail`, `body`, `expires` | 审批提示 |
| `transcript` | `request_id`, `text` | STT 回显：你说了什么 |
| `history` | `[{summary, ts}]` | 重连时重放最近 10 条 |
| `error` | `code`, `message` | 可恢复错误 |

### 3.2 插件 ⇄ Hermes

走 Desktop 插件 API（`host.request` / `host.onEvent`）。**不读 `state.db`，不读
`active_sessions.json`，不注入按键，不改 Hermes 源码。**

| 需求 | 调用 |
|---|---|
| Bot 列表 | `host.request('profiles.list')` —— 动态，不硬编码 |
| 发起对话 | `host.request('sessions.send', {bot, content})` |
| 流式回复 | `host.onEvent('message', ...)` → `reply.delta` 帧 |
| STT | `host.request('stt.transcribe', {audio})`（或 Gateway 的 VOICE 处理） |
| 审批 | `host.onEvent('approval', ...)` → `decision` 帧；用 `host.request('approval.resolve')` 回答 |
| TTS | `host.request('tts.synthesize', {text})` → PCM → ADPCM → 设备 |

> 具体的 Gateway 方法名在 Phase 1 对着真实插件 API 确认。**若某个名字不存在，那是要上报的
> 阻塞项，不是绕过去读内部状态的理由。**

### 3.3 摘要抽取

插件在发送前抽好摘要，固件永远不解析 Markdown：

1. 剥掉 HTML 注释（`<!--SPK ...-->` 是播报要点块，屏幕上不能显示）。
2. 剥掉围栏代码块、行内代码标记、图片与链接语法（保留链接文字）。
3. 取第一段；若有 `TITLE:` 行则取该行。
4. 硬上限 **512 字节 UTF-8**（约 170 汉字）。在字符边界截断，绝不在字形中间截。

只取最终答复：`finish_reason == 'stop'`。中途的 assistant 文本
（`finish_reason == 'tool_calls'`）不得上屏。

---

## 4. 内存预算（ESP32-C3，无 PSRAM）

| 项 | 预算 | 说明 |
|---|---|---|
| 摘要文本缓冲 | 512 B | 约 170 汉字 |
| 换行输出缓冲 | 768 B | 换行会插入 `\n` |
| 历史环形（10 × 摘要） | 5.1 KB | 仅内存；桥重连时重放 |
| 录音块 | 1 KB | 512 采样 × 16 bit |
| ADPCM 工作集 | 1 KB | 编码器 + 解码器状态 |
| LVGL 画布 | 38 KB | 2-bit 调色板索引 |
| CJK 字体（16 px） | 约 10 KB | 生成的子集 |
| **合计新增** | **约 56 KB** | |

前人实测警告（同一块板）：把文本缓冲提到 3 KB 以上**再加** 10 × 1 KB 历史，会导致
**静默停止 BLE 广播** —— 不报编译错、不开机崩溃，就是不广播。务必守住这个预算。

---

## 5. 音频链路

```
麦 ─▶ ES8311 ─▶ 16 kHz PCM16 ─▶ 512 采样块 ─▶ IMA-ADPCM ─▶ BLE 帧
```

| 属性 | 取值 | 原因 |
|---|---|---|
| 采样率 | 16 kHz 单声道 | 匹配 STT 期望 |
| 块大小 | 512 采样（1 KB PCM） | 约 32 ms；够小可实时流 |
| 编码 | **IMA-ADPCM** | 每 2 采样 1 字节 → BLE 流量比 PCM16 **少 4 倍** |
| 最长语音 | 30 s | 设备侧硬停 |

**溢出保护（来自前人经验，必须实现）：** IMA-ADPCM 是**有状态**差分编码。BLE 环形缓冲
丢一个字节，后面每个采样都解错、外推成满量程尖峰 —— 戴的人会听到一声爆响。
溢出时：**重置解码状态并丢弃那批数据**。几毫秒静音远好过一阵噪音。

**幻听守卫：** whisper 在静音段会吐套话（"中文字幕志愿者 李沛"）。
命中套话时**绝不能整段丢弃** —— **只裁尾巴**。

---

## 6. 分阶段计划

| 阶段 | 交付物 | 依赖 | 验证方式 |
|---|---|---|---|
| **0** | 本设计 + UI/UX 文档 | — | review |
| **1** | 伴侣插件 MVP：BLE 链路、bot 列表、文本往返、STT | 插件 API 方法名 | 设备：发文本，看到回复 |
| **2** | 摘要抽取 + 历史环形 + 新 UI 状态机 | Phase 1 | host tests + 设备 |
| **3** | IMA-ADPCM + 溢出守卫 + 提示音 + 电池 + 设置/NVS | Phase 2 | host tests + 设备 |
| **4** | 决策审批 + 设备认证（HMAC 挑战） | Phase 1 | host tests + 设备 |
| **5** | host test 套件 + `tools/validate.sh` 门禁 + CI | 全部 | `./tools/validate.sh` |

每阶段结束按 `AGENTS.md` 出交付报告：

```text
Build: PASS / FAIL / NOT RUN
Host tests: PASS / FAIL / NOT RUN
Device tests: PASS / FAIL / NOT RUN
Unverified: remaining board, instrument, or user checks
```

---

## 7. 借了什么，刻意不借什么

### 从 `ai-passport-hermes` 借用

| 项 | 落点 |
|---|---|
| 产品定义（摘要 / 拍板 / 可扫读） | UI-UX 文档 §1 |
| 中文断行：按字符断、整字符量宽、避头尾 | `text_layout.c` + host test |
| 行距是 **21 px** 而非 10 px | `text_layout.c` 常量 |
| 换行输出 768 B / 输入 512 B | §4 |
| IMA-ADPCM + 溢出重置解码状态 | `adpcm.c` + host test |
| 幻听守卫只裁尾巴 | `summary_extract.c` |
| 只推摘要不推全文（BLE 无确认写入） | §3.3 |
| 历史环形 10 条 + 重连重放 | `history_ring.c` |
| 只取最终答复（`finish_reason == 'stop'`） | §3.3 |
| 提示音（880 / 1760 Hz 节奏） | UI-UX §8 |
| 设置设备 ⇄ 桥双向镜像 | `nvs_settings.c` |

### 刻意**不**借用

| 项 | 原因 |
|---|---|
| 直接读 `state.db` | schema 未公开；Hermes 升级即断 |
| 读 `runtime/active_sessions.json` | 未公开契约 |
| 按键注入（`keybd_event`） | 脆弱：依赖焦点、窗口标题、快捷键 |
| 剪贴板 + Ctrl+V 发送 | 覆盖用户剪贴板；需要前台 |
| 改 Hermes `web_routers/audio.py` | Hermes 升级即静默失效 |
| 硬编码 `%LOCALAPPDATA%\hermes\tools\ffmpeg-9.0.1-...` | 钉死工具版本 |
| 硬编码 `C:\Users\Administrator\...` 回退路径 | 别人的机器用户名 |

### 从 `hermes-gadget-sdk` 借用

| 项 | 阶段 |
|---|---|
| 设备身份：`id = "hg-" + sha256(key)[:16]` | 4 |
| 认证：HMAC-SHA256 over `context|device_id|nonce`，每次连接新 nonce | 4 |
| 认证与 OTA 上下文域分隔 | 4 |
| 常量时间比较（`hmac.compare_digest`） | 4 |
| 协议版本字段 + 不匹配即拒 | 1 |
| 二进制帧头 `[channel][stream][seq]` | 1 |

---

## 8. 不得回退的约束

来自 `AGENTS.md`：

- 保护 Flash 布局：3 MB 应用上限，`cardid` 在 `0x356000`。
- LVGL：LVGL 任务之外的代码访问 LVGL 对象必须持 `bsp_lvgl_lock()`。按键回调不得阻塞。
- BSP / 应用分层：板级逻辑进 `components/bsp`，页面/状态机进 `main`。
- 绝不提交凭据、设备密钥、未脱敏日志。
- 双语文档：英文在默认路径，`.zh_CN.md` 配对。

---

## 9. 待主人确认的问题

1. **Phase 1 的 Gateway 方法名** —— 编码前对着真实 Desktop 插件 API 确认。
   若 `sessions.send` / `stt.transcribe` 不存在，我们**上报阻塞**，而不是退回读内部状态。
2. **设备端要不要语音播报** —— 板子有喇叭（ES8311 全双工）。工牌要念摘要吗
   （ai-passport-hermes 念，200 字上限），还是保持安静？
3. **多 bot** —— 保留 bot 列表（最多 8 个），还是固定单个默认 bot、去掉列表 UI？
