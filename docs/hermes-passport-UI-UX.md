# hermes-passport UI/UX Design

[简体中文](hermes-passport-UI-UX.zh_CN.md) | **English**

Target: **FoloToy AI Passport** — ESP32-C3, ST7789P3 240×320 portrait RGB565, 3 buttons
(`UP` / `DOWN` / `OK`, shared GPIO0 ADC resistor ladder), ES8311 audio, CW2017 fuel gauge.

Design intent follows the product definition validated by `ai-passport-hermes`:
**the badge answers "what is the Agent doing, is it done, do I need to decide something" —
while the wearer is away from the desk.** It shows summaries, not full text.

---

## 1. Design principles

| # | Principle | Consequence |
|---|---|---|
| 1 | **Summary, never full text** | BLE unacknowledged writes drop fragments; a long payload is lost whole. Keep one screen ≤ ~170 CJK chars. |
| 2 | **One thumb, three keys** | Every state must be operable with UP/DOWN/OK only. No key chord is required for a core action. |
| 3 | **Glanceable in 2 seconds** | Headline + one detail line on hero screens. The wearer reads it while walking. |
| 4 | **Decisions first** | When Hermes asks for approval, the badge takes over the screen. Approving is one keypress. |
| 5 | **Quiet by default** | Neutral greys. Colour is reserved for *semantic* signals (recording, error, decision). |
| 6 | **Destructive actions need a hold** | Reset / forget pairing / new session require a 2 s long-press, never a click. |

---

## 2. Colour scheme

Cool monochrome, matching the Hermes desktop theme. Colour appears only for meaning.

| Token | Hex | Use |
|---|---|---|
| `COLOR_BG` | `#080A0C` | Screen background |
| `COLOR_PANEL` | `#1A1D21` | Card / message panel |
| `COLOR_TEXT` | `#F2F4F6` | Primary text |
| `COLOR_TEXT_DIM` | `#8B9198` | Hints, timestamps, page counters |
| `COLOR_ACCENT` | `#C3C7CC` | Selected row, focus bar |
| `COLOR_RECORD` | `#D97757` | Recording indicator **only** |
| `COLOR_WARN` | `#C9A227` | Decision prompt, low battery **only** |
| `COLOR_ERROR` | `#B54A4A` | Error state **only** |

Fonts (LVGL 9, enabled in `sdkconfig.defaults`):

| Role | Font | Size |
|---|---|---|
| Status bar | `lv_font_montserrat_12` | 12 px |
| Hint bar | `lv_font_montserrat_12` | 12 px |
| Body / summary | `lv_font_unscii_8` + CJK fallback | 16 px line height (measured 21 px baseline advance) |
| Headline | `lv_font_montserrat_20` | 20 px |

> CJK body text uses the generated 16 px CJK font. Latin-only chrome uses Montserrat.

---

## 3. Screen anatomy (all states)

```
┌──────────────────────────────┐  y=0
│ BLE▲  10:24        ▮▮▮▯  82% │  status bar   20 px
├──────────────────────────────┤  y=20
│                              │
│         CONTENT AREA         │  240 × 280 px
│                              │
│                              │
├──────────────────────────────┤  y=300
| OK talk / ^ home / hold menu |  hint bar     20 px
└──────────────────────────────┘  y=320
```

- **Status bar** (20 px): BLE link glyph (bright = connected, mid grey = advertising), clock, battery cells.
- **Content area** (280 px): hero layout or text layout depending on state.
- **Hint bar** (20 px): exactly one line, always tells the user what each key does *right now*.

---

## 4. Screens

### 4.1 HOME — hero layout

```
┌──────────────────────────────┐
│ BLE▲  10:24        ▮▮▮▯  82% │
│                              │
│                              │
│          ┌────────┐          │
│          │        │          │
│          │ NOUS   │          │
│          │  GIRL  │          │
│          │180×180 │          │
│          └────────┘          │
│                              │
│      Hi, I'm Hermes         │  headline (20 px)
|      Press OK to talk       |  detail (16 px, dim)
|                              |
+------------------------------+
| OK talk / ^ home / hold menu |
+------------------------------+
```

- Mascot: 180×180, 2-bit palette-indexed (4 greys) — generated from the official Nous Girl SVG, 4× supersampled.
- Shows the **last reply headline** after a turn completes; returns to "Hi, I'm Hermes" after 20 s idle.

### 4.2 SUMMARY — reply view (the primary working screen)

```
┌──────────────────────────────┐
| BLE^  10:24       [#####] 82%|
| Lisbon weather tmrw    [3/7] |  headline + page counter
+------------------------------+
| Cloudy turning fine, 18-25C, |
| easterly force 3. Good for   |
| being outdoors, but dress in |
| layers for the chill of the  |
| morning and evening.         |
|                              |
| Source: weather tool         |  dim
|                              |
| - - - - - - - - - - - - - -  |  separator
| > Prev: NAS backup    10:12  |  history preview
+------------------------------+
| ^ scroll / v page / OK talk  |
+------------------------------+
```

- Body text **wraps by character** (CJK has no spaces). Width is measured over whole characters.
- **Kinsoku (line-head control):** a line never *starts* with closing punctuation. Implemented by moving the final
  character of the current line down to the next one — never by over-filling the line (the renderer
  clips over-wide content and the punctuation would vanish with the previous glyph).
- Page counter `[n/m]` top-right; long replies page themselves about 1 s per line when idle.

### 4.3 HISTORY — ring buffer of the last 10 summaries

```
┌──────────────────────────────┐
| BLE^  10:24       [#####] 82%|
| History                 [2/10|
+------------------------------+
| > Lisbon weather tmrw  10:24 |  <- selected (accent bar)
|   NAS backup           10:12 |
|   PR #42 merged        09:58 |
|   Deploy finished      09:31 |
|   Disk alert cleared   08:47 |
|                              |
+------------------------------+
| ^v select / OK open / hold ba|
+------------------------------+
```

- Ring buffer, newest first. The same reply never occupies two slots.
- Device storage is RAM-only; the bridge **replays the last 10** on every reconnect.

### 4.4 RECORDING

```
┌──────────────────────────────┐
│ BLE▲  10:24        ▮▮▮▯  82% │
│                              │
│                              │
|            ***               |  recording dot (COLOR_RECORD)
|                              |
|       Listening... (0:03)    |
|                              |
|      ################        |  live level meter (16 bars)
|      _-_/=\/=/_-_/=\/=\_     |
|                              |
+------------------------------+
| OK stop / hold OK to cancel  |
+------------------------------+
```

- Max 30 s. Auto-stop at the limit.
- Level meter drawn from the last 16 RMS blocks (32 ms each).
- A tap under 350 ms is discarded as an accidental press.

### 4.5 TRANSCRIBE — confirm before sending

```
┌──────────────────────────────┐
│ BLE▲  10:24        ▮▮▮▯  82% │
| Confirm transcript      [1/2]|
+------------------------------+
| Could you check tomorrow's   |
| weather in Lisbon, and see   |
| whether I need an umbrella?  |
|                              |
|        ~ more below ~        |
|                              |
+------------------------------+
| ^ redo / v page / OK send    |
+------------------------------+
```

- The wearer confirms what Hermes heard before it enters the session.
- `UP` = re-record, `DOWN` = page through a long transcript, `OK` = send.

### 4.6 DECISION — approval takes over the screen

```
┌──────────────────────────────┐
│ BLE▲  10:24        ▮▮▮▯  82% │
| ! Your call needed           |
+------------------------------+
|                              |
|   Allow this command?        |
|                              |
|   git push --force           |  mono, panel bg
|                              |
|   Target: production         |  dim detail
|                              |
|                              |
|         +-------+            |
|         |CONFIRM|  <- OK     |
|         +-------+            |
|         +-------+            |
|         |DENY   |  <- hold   |
|         +-------+            |
+------------------------------+
| OK confirm / hold OK to deny |
+------------------------------+
```

- Covers: dangerous shell commands, `/new`, model switches, anything Hermes asks to confirm.
- Prompts **queue** one at a time, in arrival order. A prompt received while offline shows on reconnect.
- On timeout the badge reports "That question has expired".

### 4.7 MENU / SETTINGS

```
┌──────────────────────────────┐
│ BLE▲  10:24        ▮▮▮▯  82% │
| Settings                     |
+------------------------------+
| > Brightness  ########--  80 |
|   Backlight            on    |
|   Volume       #####----- 45 |
|   Speak reply          on    |
|   Hint sounds          on    |
|   Bluetooth            on    |
|   Screen off           now   |
|   Reset                ...   |
+------------------------------+
| ^v select / OK adjust / hold |
+------------------------------+
```

| Item | Values | Persists to NVS |
|---|---|---|
| Brightness | 10–100 % | yes |
| Backlight | on / off (off = sleep) | yes |
| Volume | 0–100 % | yes |
| Speak reply | on / off (read the summary aloud) | yes |
| Hint sounds | on / off | yes |
| Bluetooth | on / off | yes |
| Screen off | now / 30 s / 60 s / never | yes |
| Reset | factory defaults (**long-press to confirm**) | — |

- Applied at boot from NVS. `OK` cycles or steps a value; `long-press UP/DOWN` jumps to min/max.
- The Speak-reply and Hint-sound switches mirror the bridge's truth (the bridge executes them) and sync both ways.

### 4.8 SLEEP — screen off, everything else on

```
┌──────────────────────────────┐
│                              │
|        (all black)           |
│                              │
│                              │
└──────────────────────────────┘
```

- Only the backlight turns off. CPU, BLE and audio keep running.
- Any key wakes it. The wake press does *nothing else* — no accidental action.

---

## 5. State machine

```
                        ┌──────────────┐
        boot ─────────▶ │   PAIRING    │◀──────────── unpaired / link lost
                        └──────┬───────┘
                               │ paired + connected
                               ▼
                        ┌──────────────┐
          ┌────────────▶│     HOME     │◀────────────┐
          │             └──┬───┬───┬───┘             │
          │      OK click  │   │   │  idle 20 s      │
          │                ▼   │   ▼                 │
          │        ┌─────────┐ │ ┌─────────┐         │
          │        │RECORDING│ │ │ SUMMARY │─────────┤
          │        └────┬────┘ │ └────┬────┘         │
          │   OK stop   │      │      │ ▼ long press │
          │             ▼      │      ▼              │
          │      ┌────────────┐│ ┌─────────┐         │
          │      │ TRANSCRIBE ││ │ HISTORY │─────────┘
          │      └──────┬─────┘│ └────┬────┘
          │        OK   │      │      │ OK (select)
          │             ▼      │      ▼
          │        ┌─────────┐ │  ┌─────────┐
          └────────│  HOME   │ │  │ SUMMARY │
                   └─────────┘ │  └─────────┘
                               │
                    any prompt │ ▼
                        ┌──────────────┐
                        |   DECISION   |-- OK / hold-OK --> previous state
                        +--------------+
        hold OK (2 s) from HOME --> MENU -- hold OK --> HOME
        any state idle (configurable) --> SLEEP -- any key --> previous state
```

---

## 6. Interaction table (button × state)

| State | `UP` click | `UP` long | `DOWN` click | `DOWN` long | `OK` click | `OK` long |
|---|---|---|---|---|---|---|
| **HOME** | scroll up (to headline) | **MENU** | open HISTORY | — | start RECORDING | **new session** (2 s confirm) |
| **SUMMARY** | scroll body up 1 line | HOME | next history entry | HISTORY | start RECORDING | new session (2 s confirm) |
| **HISTORY** | select previous | HOME | select next | SUMMARY | open SUMMARY | HOME |
| **RECORDING** | — | — | — | — | stop & go TRANSCRIBE | **cancel** (discard) |
| **TRANSCRIBE** | re-record | — | page transcript | — | send → HOME | re-record |
| **DECISION** | — | — | — | — | **confirm** | **deny** |
| **MENU** | previous item / value − | HOME | next item / value + | — | step / toggle value | HOME |
| **SLEEP** | wake | wake | wake | wake | wake | wake |

**Rules**

1. A long-press always does the *safer* thing (cancel, reject, back).
2. No state requires two keys held together.
3. Every state's hint bar restates the mapping — the user never has to remember it.

---

## 7. Status bar states

| Glyph | Meaning |
|---|---|
| `BLE▲` bright | connected & paired |
| `BLE▲` mid grey | advertising, not connected |
| `BLE▲` dim | Bluetooth off |
| `▮▮▮▯` + `%` | battery cells from CW2017; **not drawn at all** if the gauge reports no reading |
| `⚠` in `COLOR_WARN` | low battery (< 15 %), or a decision is pending |
| `●` in `COLOR_RECORD` | recording |

---

## 8. Feedback sounds (ES8311, optional)

| Event | Pattern | Frequency |
|---|---|---|
| Thinking / working | 1 short beep every 3 s | 880 Hz |
| Start recording | 1 beep | 1760 Hz |
| Stop recording | 2 beeps | 1760 Hz |
| Decision pending | 1 rising beep | 660→1320 Hz |

All gated by the Hint-sounds setting. Silent mode still shows the visual indicator.

---

## 9. Accessibility / constraints

- Body line advance measured at **21 px**, not 10 px — scroll limits must use the measured value.
- Output buffer for wrapped text needs **768 B** (input 512 B) because wrapping inserts `\n`.
- Total text buffer budget **512 B** for one summary (~170 CJK chars). Raising it past ~3 KB plus
  history RAM caused silent BLE advertising failure on the C3 (no PSRAM) in prior art — keep it small.
- Audio: record in 512-sample blocks (1 KB). Never buffer a full recording longer than 3 s.

---

## 10. Open questions for review

1. **Which screen is the default?** — **DECIDED**: HOME shows the most recent summary. The mascot appears only when there is nothing to show yet (first boot, or after a new session), and a summary falls back to the mascot after 20 s idle.
2. **Speak replies on by default?** (ai-passport-hermes defaults to on, 200-character cap.)
3. **Should the device start a new session?** (Current design: long-press OK for 2 s, which runs the Hermes `/new` confirmation flow.)
