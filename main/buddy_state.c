// main/buddy_state.c — pure UI state machine. See buddy_state.h.
//
// The table in bs_key() IS the decision table from
// docs/hermes-passport-UI-UX.md section 6. When the two disagree, the
// document wins and this file is the bug.

#include "buddy_state.h"

// ── Names (logs and tests) ──────────────────────────────────────────────────

static const char *const kStateNames[BS_STATE_COUNT] = {
    "PAIRING", "HOME", "SUMMARY", "HISTORY", "RECORDING",
    "TRANSCRIBE", "DECISION", "MENU", "SLEEP",
};

static const char *const kActionNames[BS_ACT_COUNT] = {
    "NONE", "SCROLL_UP", "SCROLL_DOWN", "SELECT_PREV", "SELECT_NEXT",
    "OPEN_HISTORY", "OPEN_SUMMARY", "START_RECORDING", "STOP_RECORDING",
    "CANCEL_RECORDING", "SEND_TRANSCRIPT", "RERECORD", "PAGE_TRANSCRIPT",
    "CONFIRM", "DENY", "NEW_SESSION", "MENU_PREV", "MENU_NEXT", "MENU_STEP",
    "MENU_EXIT", "WAKE", "SLEEP", "GO_HOME", "SHOW_MASCOT",
};

const char *bs_state_name(bs_state_t st)
{
    if ((int)st < 0 || (int)st >= BS_STATE_COUNT) return "?";
    return kStateNames[st];
}

const char *bs_action_name(bs_action_t act)
{
    if ((int)act < 0 || (int)act >= BS_ACT_COUNT) return "?";
    return kActionNames[act];
}

// ── Init ────────────────────────────────────────────────────────────────────

void bs_init(bs_t *s)
{
    if (!s) return;
    s->state = BS_PAIRING;
    s->has_summary = false;
    s->recording = false;
    s->transcript_ready = false;
    s->decision_pending = false;
    s->history_count = 0;
    s->history_index = 0;
    s->menu_index = 0;
    s->menu_count = 0;
    s->return_state = BS_HOME;
}

// ── Small helpers ───────────────────────────────────────────────────────────

// Return to the newest summary, clamping the cursor if entries were dropped.
static void clamp_history(bs_t *s)
{
    if (s->history_count < 0) s->history_count = 0;
    if (s->history_count > BS_HISTORY_MAX) s->history_count = BS_HISTORY_MAX;
    if (s->history_index < 0) s->history_index = 0;
    if (s->history_count > 0 && s->history_index >= s->history_count) {
        s->history_index = s->history_count - 1;
    }
}

// ── Keys ────────────────────────────────────────────────────────────────────

bs_action_t bs_key(bs_t *s, bs_key_t key, bs_ev_t ev)
{
    if (!s) return BS_ACT_NONE;

    // SLEEP: any key wakes and does nothing else. The wake press must never
    // also trigger the action it would have triggered while awake.
    if (s->state == BS_SLEEP) {
        s->state = s->return_state;
        return BS_ACT_WAKE;
    }

    bool is_long = (ev == BS_EV_LONG);

    switch (s->state) {

    // ── PAIRING: no input is meaningful until the link is up ──────────────
    case BS_PAIRING:
        return BS_ACT_NONE;

    // ── HOME: newest summary, or the mascot ───────────────────────────────
    case BS_HOME:
        if (key == BS_KEY_UP && !is_long) return BS_ACT_SCROLL_UP;
        if (key == BS_KEY_UP && is_long) { s->state = BS_MENU; return BS_ACT_MENU_EXIT; }
        if (key == BS_KEY_DOWN && !is_long) {
            if (s->history_count > 0) { s->state = BS_HISTORY; return BS_ACT_OPEN_HISTORY; }
            return BS_ACT_NONE;
        }
        if (key == BS_KEY_DOWN && is_long) return BS_ACT_NONE;
        if (key == BS_KEY_OK && !is_long) {
            s->state = BS_RECORDING;
            s->recording = true;
            s->transcript_ready = false;
            return BS_ACT_START_RECORDING;
        }
        if (key == BS_KEY_OK && is_long) return BS_ACT_NEW_SESSION;
        return BS_ACT_NONE;

    // ── SUMMARY: reading one reply ────────────────────────────────────────
    case BS_SUMMARY:
        if (key == BS_KEY_UP && !is_long) return BS_ACT_SCROLL_UP;
        if (key == BS_KEY_UP && is_long) { s->state = BS_HOME; return BS_ACT_GO_HOME; }
        if (key == BS_KEY_DOWN && !is_long) {
            // Step to the next (older) entry in the ring.
            if (s->history_count > 0 && s->history_index + 1 < s->history_count) {
                s->history_index++;
                return BS_ACT_SELECT_NEXT;
            }
            return BS_ACT_NONE;
        }
        if (key == BS_KEY_DOWN && is_long) {
            if (s->history_count > 0) { s->state = BS_HISTORY; return BS_ACT_OPEN_HISTORY; }
            return BS_ACT_NONE;
        }
        if (key == BS_KEY_OK && !is_long) {
            s->state = BS_RECORDING;
            s->recording = true;
            s->transcript_ready = false;
            return BS_ACT_START_RECORDING;
        }
        if (key == BS_KEY_OK && is_long) return BS_ACT_NEW_SESSION;
        return BS_ACT_NONE;

    // ── HISTORY: picking from the ring ────────────────────────────────────
    case BS_HISTORY:
        if (key == BS_KEY_UP && !is_long) {
            if (s->history_index > 0) { s->history_index--; return BS_ACT_SELECT_PREV; }
            return BS_ACT_NONE;
        }
        if (key == BS_KEY_UP && is_long) { s->state = BS_HOME; return BS_ACT_GO_HOME; }
        if (key == BS_KEY_DOWN && !is_long) {
            if (s->history_index + 1 < s->history_count) {
                s->history_index++;
                return BS_ACT_SELECT_NEXT;
            }
            return BS_ACT_NONE;
        }
        if (key == BS_KEY_DOWN && is_long) { s->state = BS_SUMMARY; return BS_ACT_OPEN_SUMMARY; }
        if (key == BS_KEY_OK && !is_long) { s->state = BS_SUMMARY; return BS_ACT_OPEN_SUMMARY; }
        if (key == BS_KEY_OK && is_long) { s->state = BS_HOME; return BS_ACT_GO_HOME; }
        return BS_ACT_NONE;

    // ── RECORDING ─────────────────────────────────────────────────────────
    case BS_RECORDING:
        // A long press is the safer action: discard, never send.
        if (key == BS_KEY_OK && is_long) {
            s->state = BS_HOME;
            s->recording = false;
            return BS_ACT_CANCEL_RECORDING;
        }
        if (key == BS_KEY_OK && !is_long) {
            s->state = BS_TRANSCRIBE;
            s->recording = false;
            return BS_ACT_STOP_RECORDING;
        }
        return BS_ACT_NONE;

    // ── TRANSCRIBE: confirm what was heard ────────────────────────────────
    case BS_TRANSCRIBE:
        if (key == BS_KEY_UP && !is_long) {
            s->state = BS_RECORDING;
            s->recording = true;
            s->transcript_ready = false;
            return BS_ACT_RERECORD;
        }
        if (key == BS_KEY_UP && is_long) return BS_ACT_NONE;
        if (key == BS_KEY_DOWN && !is_long) return BS_ACT_PAGE_TRANSCRIPT;
        if (key == BS_KEY_DOWN && is_long) return BS_ACT_NONE;
        if (key == BS_KEY_OK && !is_long) {
            s->state = BS_HOME;
            s->transcript_ready = false;
            return BS_ACT_SEND_TRANSCRIPT;
        }
        if (key == BS_KEY_OK && is_long) {
            // Safer than sending: throw the transcript away and speak again.
            s->state = BS_RECORDING;
            s->recording = true;
            s->transcript_ready = false;
            return BS_ACT_RERECORD;
        }
        return BS_ACT_NONE;

    // ── DECISION: Hermes is waiting for an approval ───────────────────────
    case BS_DECISION:
        // Long press denies: the destructive answer must be deliberate.
        if (key == BS_KEY_OK && is_long) {
            s->state = s->return_state;
            s->decision_pending = false;
            return BS_ACT_DENY;
        }
        if (key == BS_KEY_OK && !is_long) {
            s->state = s->return_state;
            s->decision_pending = false;
            return BS_ACT_CONFIRM;
        }
        return BS_ACT_NONE;

    // ── MENU: settings ────────────────────────────────────────────────────
    case BS_MENU:
        if (key == BS_KEY_UP && !is_long) {
            if (s->menu_index > 0) s->menu_index--;
            return BS_ACT_MENU_PREV;
        }
        if (key == BS_KEY_UP && is_long) { s->state = BS_HOME; return BS_ACT_MENU_EXIT; }
        if (key == BS_KEY_DOWN && !is_long) {
            if (s->menu_count > 0 && s->menu_index + 1 < s->menu_count) s->menu_index++;
            return BS_ACT_MENU_NEXT;
        }
        if (key == BS_KEY_DOWN && is_long) return BS_ACT_NONE;
        if (key == BS_KEY_OK && !is_long) return BS_ACT_MENU_STEP;
        if (key == BS_KEY_OK && is_long) { s->state = BS_HOME; return BS_ACT_MENU_EXIT; }
        return BS_ACT_NONE;

    default:
        return BS_ACT_NONE;
    }
}

// ── Link / timer events ─────────────────────────────────────────────────────

bs_action_t bs_notify(bs_t *s, bs_notify_t ev)
{
    if (!s) return BS_ACT_NONE;

    switch (ev) {

    case BS_NOTIFY_LINK_UP:
        if (s->state == BS_PAIRING) s->state = BS_HOME;
        return BS_ACT_NONE;

    case BS_NOTIFY_LINK_DOWN:
        // Everything is lost with the link: stop recording, drop the decision.
        s->recording = false;
        s->decision_pending = false;
        s->transcript_ready = false;
        s->has_summary = false;
        s->state = BS_PAIRING;
        return BS_ACT_NONE;

    case BS_NOTIFY_SUMMARY:
        s->has_summary = true;
        clamp_history(s);
        // A new reply pulls the wearer back to the newest thing, unless they
        // are mid-task: recording and approvals must not be interrupted.
        if (s->state == BS_HOME || s->state == BS_SUMMARY || s->state == BS_HISTORY) {
            s->history_index = 0;
            s->state = s->has_summary ? BS_SUMMARY : BS_HOME;
            return BS_ACT_NONE;
        }
        return BS_ACT_NONE;

    case BS_NOTIFY_DECISION:
        // Take over the screen, remembering where to return.
        if (s->state != BS_DECISION) s->return_state = s->state;
        s->decision_pending = true;
        s->state = BS_DECISION;
        return BS_ACT_NONE;

    case BS_NOTIFY_DECISION_DONE:
        // Answered from another chat, or it expired.
        if (s->state == BS_DECISION) s->state = s->return_state;
        s->decision_pending = false;
        return BS_ACT_NONE;

    case BS_NOTIFY_TRANSCRIPT:
        s->transcript_ready = true;
        return BS_ACT_NONE;

    case BS_NOTIFY_IDLE:
        if (s->state == BS_SLEEP) return BS_ACT_NONE;
        // Idle drops the wearer back to the mascot, or sleeps if configured
        // so. Returning to HOME keeps the summary visible until the mascot
        // timeout; the renderer decides which to draw from has_summary.
        s->return_state = s->state;
        s->state = BS_SLEEP;
        return BS_ACT_SLEEP;

    default:
        return BS_ACT_NONE;
    }
}
