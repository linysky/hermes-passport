# hermes-passport Technical Design

[简体中文](hermes-passport-TECHNICAL_DESIGN.zh_CN.md) | **English**

Companion to [hermes-passport-UI-UX.md](hermes-passport-UI-UX.md). This document covers
architecture, contracts, memory budget and the phased implementation plan.

Reference inputs: `ai-passport-hermes` (product definition + hardware lessons) and
`hermes-gadget-sdk` (device auth + protocol discipline). See
`../../docs/ai-passport-hermes-review.md` and `../../docs/hermes-gadget-sdk-review.md`.

---

## 1. Architecture

```
┌────────────────────────────┐     BLE GATT      ┌──────────────────────────┐
│  AI Passport (ESP32-C3)    │◀─────────────────▶│  Companion plugin        │
│                            │  Nordic UART +    │  (Hermes Desktop plugin) │
│  main/  app logic, UI      │  newline JSON +   │                          │
│  components/bsp/  drivers  │  binary voice     │  plugin.js  ─ UI pane    │
│                            │                   │  plugin_api.py ─ BLE+RPC │
└────────────────────────────┘                   └────────────┬─────────────┘
                                                              │ host.request()
                                                              │ host.onEvent()
                                                 ┌────────────▼─────────────┐
                                                 │  Hermes Gateway          │
                                                 │  STT · agent turn · TTS  │
                                                 │  profiles · sessions     │
                                                 └──────────────────────────┘
```

**The plugin is the only touchpoint with Hermes.** The firmware never learns Gateway
internals. This is the deliberate opposite of the `ai-passport-hermes` approach (reading
`state.db`, injecting keystrokes, patching Hermes source) — see §7.

---

## 2. Component breakdown

| Component | Location | Responsibility | Testable off-device |
|---|---|---|---|
| `text_layout` | `main/text_layout.c` | CJK-aware wrap, whole-char width, kinsoku | ✅ host test |
| `summary_extract` | `main/summary_extract.c` | Markdown → plain summary, SPK block extraction | ✅ host test |
| `history_ring` | `main/history_ring.c` | 10-slot ring, dedup, page selection | ✅ host test |
| `buddy_state` | `main/buddy_state.c` | UI state machine (pure, no LVGL) | ✅ host test |
| `adpcm` | `main/adpcm.c` | IMA-ADPCM encode + decoder-state reset on overflow | ✅ host test |
| `nvs_settings` | `main/nvs_settings.c` | Settings schema, load/apply/restore-defaults | ✅ host test |
| `ble_protocol` | `main/hermes_ble.c` | GATT frames, MTU chunking, sequence | shims |
| UI render | `main/hermes_bridge.c` | LVGL widgets, per state | ❌ device only |
| BSP | `components/bsp/` | display / audio / button / battery / i2c | ❌ hardware |

Per `AGENTS.md`: state machines, protocols, timing and layout calculations must live
outside ESP-IDF/LVGL and be covered by host tests. That is exactly the split above.

---

## 3. Contracts

### 3.1 BLE frame (device ⇄ plugin)

Control frames are newline-delimited JSON on the NUS TX characteristic.
Binary voice uses the RX characteristic with a 4-byte header.

```
Binary voice frame:  [kind u8][stream u8][seq u16 LE][payload ...]
  kind: 0x01 START | 0x02 DATA | 0x03 END | 0x04 CANCEL
  seq : wraps at 65535, per stream
```

Control messages (device → plugin):

| `cmd` | Payload | Meaning |
|---|---|---|
| `hello` | `proto`, `device_id`, `name`, `fw` | handshake |
| `key` | `k` (up/down/ok), `ev` (click/long) | button event |
| `setting` | `key`, `value` | device toggles a mirrored setting |
| `decision` | `prompt_id`, `verdict` (confirm/deny) | answer an approval |
| `ack` | `id`, `ok` | delivery receipt |

Control messages (plugin → device):

| `type` | Payload | Meaning |
|---|---|---|
| `welcome` | `session`, `paired`, `heartbeat_s` | handshake complete |
| `bots` | `[{id,name,status}]` | dynamic bot list (never hardcoded) |
| `reply.delta` | `request_id`, `text` | streaming text |
| `reply` | `request_id`, `text`, `summary` | final reply + pre-extracted summary |
| `status` | `text` | live status phrase ("Checking the forecast") |
| `decision` | `prompt_id`, `title`, `detail`, `body`, `expires` | approval prompt |
| `transcript` | `request_id`, `text` | STT echo of what the wearer said |
| `history` | `[{summary, ts}]` | replay of the last 10 on reconnect |
| `error` | `code`, `message` | recoverable error |

### 3.2 Plugin ⇄ Hermes

Uses the Desktop plugin API (`host.request` / `host.onEvent`). **No direct reads of
`state.db`, no `active_sessions.json`, no keystroke injection, no patches to Hermes.**

| Need | Call |
|---|---|
| Bot list | `host.request('profiles.list')` — dynamic, never hardcoded |
| Send a turn | `host.request('sessions.send', {bot, content})` |
| Stream reply | `host.onEvent('message', ...)` → `reply.delta` frames |
| STT | `host.request('stt.transcribe', {audio})` (or Gateway VOICE handling) |
| Approvals | `host.onEvent('approval', ...)` → `decision` frame; answer via `host.request('approval.resolve')` |
| TTS | `host.request('tts.synthesize', {text})` → PCM → ADPCM → device |

> Exact Gateway method names are confirmed against the live plugin API during Phase 1.
> Any name that turns out not to exist is a **blocker to raise**, not something to work
> around by reading internal state.

### 3.3 Summary extraction

The plugin extracts a summary before sending, so the firmware never parses Markdown:

1. Strip HTML comments (`<!--SPK ...-->` is the speak-brief block; the screen must not show it).
2. Strip fenced code blocks, inline code markers, image and link syntax (keep link text).
3. Take the first paragraph, or `TITLE:` line if present.
4. Hard cap **512 bytes UTF-8** (~170 CJK chars). Truncate at a character boundary, never mid-glyph.

Filter to final replies only: `finish_reason == 'stop'`. Interim assistant text
(`finish_reason == 'tool_calls'`) must not reach the screen.

---

## 4. Memory budget (ESP32-C3, no PSRAM)

| Item | Budget | Note |
|---|---|---|
| Summary text buffer | 512 B | ~170 CJK chars |
| Wrap output buffer | 768 B | wrapping inserts `\n` |
| History ring (10 × summary) | 5.1 KB | RAM only; bridge replays on reconnect |
| Audio record block | 1 KB | 512 samples × 16 bit |
| ADPCM working set | 1 KB | encoder + decoder state |
| LVGL canvas | 38 KB | 2-bit palette index |
| CJK font (16 px) | ~10 KB | generated subset |
| **Total added** | **~56 KB** | |

Prior art warning (measured on this board): raising the text buffer past ~3 KB **plus**
10 × 1 KB history caused **silent BLE advertising failure** — no compile error, no boot
crash, the device simply stops advertising. Stay inside this budget.

---

## 5. Audio path

```
mic ─▶ ES8311 ─▶ 16 kHz PCM16 ─▶ 512-sample blocks ─▶ IMA-ADPCM ─▶ BLE frames
```

| Property | Value | Why |
|---|---|---|
| Sample rate | 16 kHz mono | matches STT expectations |
| Block | 512 samples (1 KB PCM) | ~32 ms; small enough to stream live |
| Encoding | **IMA-ADPCM** | 1 byte per 2 samples → **4× less BLE traffic** than PCM16 |
| Max utterance | 30 s | device-side hard stop |

**Overflow protection (from prior art, must implement):** IMA-ADPCM is *stateful*
differential coding. If the BLE ring buffer drops a byte, every subsequent sample
decodes wrong and extrapolates to full-scale spikes — the wearer hears a loud crack.
On overflow: **reset the decoder state and discard that batch**. A few ms of silence
is far better than a burst of noise.

**Hallucination guard:** whisper emits stock phrases in silence — a well-known one is a
Chinese subtitle-credit line naming a volunteer. Never drop the whole transcript when a
phrase matches — **trim only the tail**.

---

## 6. Phased plan

| Phase | Deliverable | Depends on | Verifiable by |
|---|---|---|---|
| **0** | This design + UI/UX doc | — | review |
| **1** | Companion plugin MVP: BLE link, bot list, text round-trip, STT | plugin API names | device: send text, see reply |
| **2** | Summary extraction + HISTORY ring + new UI state machine | Phase 1 | host tests + device |
| **3** | IMA-ADPCM + overflow guard + hint sounds + battery + settings/NVS | Phase 2 | host tests + device |
| **4** | DECISION (approvals) + device auth (HMAC challenge) | Phase 1 | host tests + device |
| **5** | Host test suite + `tools/validate.sh` gate + CI | all | `./tools/validate.sh` |

Each phase ends with the delivery report required by `AGENTS.md`:

```text
Build: PASS / FAIL / NOT RUN
Host tests: PASS / FAIL / NOT RUN
Device tests: PASS / FAIL / NOT RUN
Unverified: remaining board, instrument, or user checks
```

---

## 7. What is borrowed, and what is deliberately not

### Borrowed from `ai-passport-hermes`

| Item | Where it lands |
|---|---|
| Product definition (summary / decision / glanceable) | UI-UX doc §1 |
| CJK wrap: char-break, whole-char width, kinsoku | `text_layout.c` + host test |
| Line advance is **21 px**, not 10 px | `text_layout.c` constants |
| Wrap output buffer 768 B vs input 512 B | §4 |
| IMA-ADPCM + decoder reset on overflow | `adpcm.c` + host test |
| Hallucination guard trims tail only | `summary_extract.c` |
| Summary only, not full text (BLE unack writes) | §3.3 |
| History ring 10 + replay on reconnect | `history_ring.c` |
| Final replies only (`finish_reason == 'stop'`) | §3.3 |
| Hint sounds (880 / 1760 Hz patterns) | UI-UX §8 |
| Settings mirrored device ⇄ bridge | `nvs_settings.c` |

### Deliberately **not** borrowed

| Item | Why |
|---|---|
| Reading `state.db` directly | Undocumented schema; breaks on Hermes upgrade |
| Reading `runtime/active_sessions.json` | Undocumented contract |
| Keystroke injection (`keybd_event`) | Fragile: depends on focus, window title, hotkeys |
| Clipboard + Ctrl+V to send | Overwrites the user's clipboard; needs foreground |
| Patching Hermes `web_routers/audio.py` | Silently lost on Hermes upgrade |
| Hardcoded `%LOCALAPPDATA%\hermes\tools\ffmpeg-9.0.1-...` | Pins a tool version |
| Hardcoded `C:\Users\Administrator\...` fallback | Another machine's username |

### Borrowed from `hermes-gadget-sdk`

| Item | Phase |
|---|---|
| Device identity: `id = "hg-" + sha256(key)[:16]` | 4 |
| Auth: HMAC-SHA256 over `context|device_id|nonce`, fresh nonce per connect | 4 |
| Domain separation between auth and OTA contexts | 4 |
| Constant-time compare (`hmac.compare_digest`) | 4 |
| Protocol version field + reject on mismatch | 1 |
| Binary frame header `[channel][stream][seq]` | 1 |

---

## 8. Constraints that must not regress

From `AGENTS.md`:

- Protected Flash layout: 3 MB application limit, `cardid` at `0x356000`.
- LVGL: any code outside the LVGL task holds `bsp_lvgl_lock()`. Button callbacks stay non-blocking.
- BSP vs app split: board logic in `components/bsp`, pages/state machines in `main`.
- Never commit credentials, device secrets or unsanitized logs.
- Bilingual docs: English at the default path, `.zh_CN.md` paired.

---

## 9. Open questions for review

1. **Phase 1 gateway method names** — confirm against the live Desktop plugin API before coding.
   If `sessions.send` / `stt.transcribe` don't exist, we raise it rather than fall back to
   reading internal state.
2. **Voice playback on the device** — the board has a speaker (ES8311 is full duplex). Should the
   badge speak the summary (ai-passport-hermes does, 200 char cap), or stay silent?
3. **Multi-bot** — keep the bot list (up to 8), or pin a single default bot and drop the list UI?
