// main/buddy_state.h — pure UI state machine for the badge.
//
// Pure C11, no ESP-IDF and no LVGL: this is the decision table from
// docs/hermes-passport-UI-UX.md section 6, expressed as code. Rendering and
// I/O live elsewhere; this module only answers "given this state and this
// input, what happens next and what should the rest of the firmware do".
//
// Covered by tests/test_buddy_state.c.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── States ──────────────────────────────────────────────────────────────────

typedef enum {
    BS_PAIRING = 0,   // boot, unpaired, or link lost
    BS_HOME,          // the primary screen: newest summary, or the mascot
    BS_SUMMARY,       // reading one reply, scrolling body / stepping history
    BS_HISTORY,       // picking from the last 10 summaries
    BS_RECORDING,     // capturing audio
    BS_TRANSCRIBE,    // confirming what was heard before sending
    BS_DECISION,      // Hermes asked for approval; takes over the screen
    BS_MENU,          // settings
    BS_SLEEP,         // backlight off, everything else running
    BS_STATE_COUNT
} bs_state_t;

// ── Inputs ──────────────────────────────────────────────────────────────────

typedef enum { BS_KEY_UP = 0, BS_KEY_DOWN, BS_KEY_OK, BS_KEY_COUNT } bs_key_t;
typedef enum { BS_EV_CLICK = 0, BS_EV_LONG, BS_EV_COUNT } bs_ev_t;

// Events that arrive from the link or the timer, not from a key.
typedef enum {
    BS_NOTIFY_LINK_UP = 0,   // paired and connected
    BS_NOTIFY_LINK_DOWN,     // link lost -> back to PAIRING
    BS_NOTIFY_SUMMARY,       // a new reply arrived
    BS_NOTIFY_DECISION,      // Hermes is waiting for an approval
    BS_NOTIFY_DECISION_DONE, // answered elsewhere, or it expired
    BS_NOTIFY_IDLE,          // the idle timer fired
    BS_NOTIFY_TRANSCRIPT,    // STT result arrived
    BS_NOTIFY_COUNT
} bs_notify_t;

// ── Outputs ─────────────────────────────────────────────────────────────────

typedef enum {
    BS_ACT_NONE = 0,
    BS_ACT_SCROLL_UP,
    BS_ACT_SCROLL_DOWN,
    BS_ACT_SELECT_PREV,
    BS_ACT_SELECT_NEXT,
    BS_ACT_OPEN_HISTORY,
    BS_ACT_OPEN_SUMMARY,
    BS_ACT_START_RECORDING,
    BS_ACT_STOP_RECORDING,
    BS_ACT_CANCEL_RECORDING,
    BS_ACT_SEND_TRANSCRIPT,
    BS_ACT_RERECORD,
    BS_ACT_PAGE_TRANSCRIPT,
    BS_ACT_CONFIRM,
    BS_ACT_DENY,
    BS_ACT_NEW_SESSION,
    BS_ACT_MENU_PREV,
    BS_ACT_MENU_NEXT,
    BS_ACT_MENU_STEP,
    BS_ACT_MENU_EXIT,
    BS_ACT_WAKE,
    BS_ACT_SLEEP,
    BS_ACT_GO_HOME,
    BS_ACT_SHOW_MASCOT,
    BS_ACT_COUNT
} bs_action_t;

// ── Machine ─────────────────────────────────────────────────────────────────

typedef struct {
    bs_state_t state;

    // What the screens need to know. Set by bs_notify() and read by the
    // renderer; kept here so the decision table can branch on it.
    bool has_summary;       // a reply is available to show on HOME
    bool recording;         // capture is in progress
    bool transcript_ready;  // STT produced text awaiting confirmation
    bool decision_pending;  // Hermes is waiting for an approval

    // Navigation cursors. The renderer reads them; this module moves them.
    int history_count;      // entries in the ring, 0..BS_HISTORY_MAX
    int history_index;      // selected entry in HISTORY, 0 = newest
    int menu_index;         // selected row in MENU
    int menu_count;         // rows in MENU

    // For returning from an interrupting state.
    bs_state_t return_state;   // where DECISION or SLEEP came from
} bs_t;

#define BS_HISTORY_MAX 10

// Reset to the boot state: PAIRING, nothing recorded, no summary.
void bs_init(bs_t *s);

// Feed one key event. Returns what the rest of the firmware should do.
bs_action_t bs_key(bs_t *s, bs_key_t key, bs_ev_t ev);

// Feed one link/timer event. Returns what the rest of the firmware should do.
bs_action_t bs_notify(bs_t *s, bs_notify_t ev);

// Human-readable names, for logs and tests. Never NULL.
const char *bs_state_name(bs_state_t st);
const char *bs_action_name(bs_action_t act);

#ifdef __cplusplus
}
#endif
