// tests/test_buddy_state.c — host tests for main/buddy_state.c
//
// Covers the decision table in docs/hermes-passport-UI-UX.md section 6, plus
// the two safety rules the design calls out:
//   * a long press always takes the safer action (cancel / deny / back);
//   * the wake press from SLEEP does nothing else.
//
// Build (see tools/validate.sh):
//   cc -std=c11 -Wall -Wextra -Werror -Imain
//      tests/test_buddy_state.c main/buddy_state.c

#include <stdio.h>
#include <string.h>

#include "buddy_state.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, (msg));  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#define CHECK_STATE(s, want, msg)                                            \
    do {                                                                     \
        checks++;                                                            \
        if ((s)->state != (want)) {                                          \
            fprintf(stderr, "FAIL %s:%d  %s (state=%s want=%s)\n",           \
                    __FILE__, __LINE__, (msg),                               \
                    bs_state_name((s)->state), bs_state_name(want));         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#define CHECK_ACT(got, want, msg)                                            \
    do {                                                                     \
        checks++;                                                            \
        if ((got) != (want)) {                                               \
            fprintf(stderr, "FAIL %s:%d  %s (act=%s want=%s)\n",             \
                    __FILE__, __LINE__, (msg),                               \
                    bs_action_name(got), bs_action_name(want));              \
            failures++;                                                      \
        }                                                                    \
    } while (0)

// Boot into HOME with one summary available, the common starting point.
static void setup_home(bs_t *s)
{
    bs_init(s);
    bs_notify(s, BS_NOTIFY_LINK_UP);
    bs_notify(s, BS_NOTIFY_SUMMARY);
    bs_notify(s, BS_NOTIFY_IDLE);   // then wake, so return_state is sane
    bs_key(s, BS_KEY_OK, BS_EV_CLICK);
    s->state = BS_HOME;
    s->has_summary = true;
    s->history_count = 3;
    s->history_index = 0;
}

// ── Boot and link ───────────────────────────────────────────────────────────

static void test_boot(void)
{
    bs_t s;
    bs_init(&s);

    CHECK_STATE(&s, BS_PAIRING, "boots into PAIRING");
    CHECK(!s.has_summary && !s.recording && !s.transcript_ready && !s.decision_pending,
          "no flags set at boot");

    // Nothing is meaningful before the link is up.
    CHECK_ACT(bs_key(&s, BS_KEY_OK, BS_EV_CLICK), BS_ACT_NONE, "PAIRING ignores keys");
    CHECK_ACT(bs_key(&s, BS_KEY_UP, BS_EV_LONG), BS_ACT_NONE, "PAIRING ignores long press");

    bs_notify(&s, BS_NOTIFY_LINK_UP);
    CHECK_STATE(&s, BS_HOME, "link up moves to HOME");
}

// ── HOME ────────────────────────────────────────────────────────────────────

static void test_home(void)
{
    bs_t s;

    setup_home(&s);
    CHECK_ACT(bs_key(&s, BS_KEY_UP, BS_EV_CLICK), BS_ACT_SCROLL_UP, "HOME UP scrolls");

    setup_home(&s);
    bs_key(&s, BS_KEY_UP, BS_EV_LONG);
    CHECK_STATE(&s, BS_MENU, "HOME long-UP opens MENU");

    setup_home(&s);
    CHECK_ACT(bs_key(&s, BS_KEY_DOWN, BS_EV_CLICK), BS_ACT_OPEN_HISTORY, "HOME DOWN opens history");
    CHECK_STATE(&s, BS_HISTORY, "HOME DOWN lands in HISTORY");

    setup_home(&s);
    CHECK_ACT(bs_key(&s, BS_KEY_DOWN, BS_EV_LONG), BS_ACT_NONE, "HOME long-DOWN is a no-op");

    setup_home(&s);
    CHECK_ACT(bs_key(&s, BS_KEY_OK, BS_EV_CLICK), BS_ACT_START_RECORDING, "HOME OK starts recording");
    CHECK_STATE(&s, BS_RECORDING, "HOME OK lands in RECORDING");
    CHECK(s.recording, "recording flag set");

    setup_home(&s);
    CHECK_ACT(bs_key(&s, BS_KEY_OK, BS_EV_LONG), BS_ACT_NEW_SESSION, "HOME long-OK asks for a new session");
    CHECK_STATE(&s, BS_HOME, "new session does not change state by itself");

    // No history: DOWN must not open an empty HISTORY.
    setup_home(&s);
    s.history_count = 0;
    CHECK_ACT(bs_key(&s, BS_KEY_DOWN, BS_EV_CLICK), BS_ACT_NONE, "HOME DOWN is a no-op with no history");
    CHECK_STATE(&s, BS_HOME, "HOME stays put with no history");
}

// ── SUMMARY ─────────────────────────────────────────────────────────────────

static void test_summary(void)
{
    bs_t s;

    setup_home(&s);
    s.state = BS_SUMMARY;
    CHECK_ACT(bs_key(&s, BS_KEY_UP, BS_EV_CLICK), BS_ACT_SCROLL_UP, "SUMMARY UP scrolls body");

    setup_home(&s);
    s.state = BS_SUMMARY;
    bs_key(&s, BS_KEY_UP, BS_EV_LONG);
    CHECK_STATE(&s, BS_HOME, "SUMMARY long-UP returns HOME");

    setup_home(&s);
    s.state = BS_SUMMARY;
    CHECK_ACT(bs_key(&s, BS_KEY_DOWN, BS_EV_CLICK), BS_ACT_SELECT_NEXT, "SUMMARY DOWN steps history");
    CHECK(s.history_index == 1, "history cursor advanced");

    // At the oldest entry, DOWN does not walk off the ring.
    setup_home(&s);
    s.state = BS_SUMMARY;
    s.history_index = s.history_count - 1;
    CHECK_ACT(bs_key(&s, BS_KEY_DOWN, BS_EV_CLICK), BS_ACT_NONE, "SUMMARY DOWN stops at the oldest");
    CHECK(s.history_index == s.history_count - 1, "cursor unchanged at the edge");

    setup_home(&s);
    s.state = BS_SUMMARY;
    bs_key(&s, BS_KEY_DOWN, BS_EV_LONG);
    CHECK_STATE(&s, BS_HISTORY, "SUMMARY long-DOWN opens HISTORY");

    setup_home(&s);
    s.state = BS_SUMMARY;
    CHECK_ACT(bs_key(&s, BS_KEY_OK, BS_EV_CLICK), BS_ACT_START_RECORDING, "SUMMARY OK records");
}

// ── HISTORY ─────────────────────────────────────────────────────────────────

static void test_history(void)
{
    bs_t s;

    setup_home(&s);
    s.state = BS_HISTORY;
    CHECK_ACT(bs_key(&s, BS_KEY_DOWN, BS_EV_CLICK), BS_ACT_SELECT_NEXT, "HISTORY DOWN selects older");
    CHECK(s.history_index == 1, "cursor moved to the older entry");

    CHECK_ACT(bs_key(&s, BS_KEY_UP, BS_EV_CLICK), BS_ACT_SELECT_PREV, "HISTORY UP selects newer");
    CHECK(s.history_index == 0, "cursor moved back to the newest");

    // Boundaries: no walking off either end.
    CHECK_ACT(bs_key(&s, BS_KEY_UP, BS_EV_CLICK), BS_ACT_NONE, "HISTORY UP stops at the newest");
    CHECK(s.history_index == 0, "cursor clamped at the newest");

    s.history_index = s.history_count - 1;
    CHECK_ACT(bs_key(&s, BS_KEY_DOWN, BS_EV_CLICK), BS_ACT_NONE, "HISTORY DOWN stops at the oldest");
    CHECK(s.history_index == s.history_count - 1, "cursor clamped at the oldest");

    s.history_index = 1;
    CHECK_ACT(bs_key(&s, BS_KEY_OK, BS_EV_CLICK), BS_ACT_OPEN_SUMMARY, "HISTORY OK opens the entry");
    CHECK_STATE(&s, BS_SUMMARY, "HISTORY OK lands in SUMMARY");

    s.state = BS_HISTORY;
    CHECK_ACT(bs_key(&s, BS_KEY_OK, BS_EV_LONG), BS_ACT_GO_HOME, "HISTORY long-OK goes HOME");
    CHECK_STATE(&s, BS_HOME, "long-OK lands in HOME");

    s.state = BS_HISTORY;
    CHECK_ACT(bs_key(&s, BS_KEY_UP, BS_EV_LONG), BS_ACT_GO_HOME, "HISTORY long-UP goes HOME");
    CHECK_STATE(&s, BS_HOME, "long-UP lands in HOME");
}

// ── RECORDING: long press is the safer action ───────────────────────────────

static void test_recording(void)
{
    bs_t s;

    setup_home(&s);
    s.state = BS_RECORDING;
    s.recording = true;
    CHECK_ACT(bs_key(&s, BS_KEY_OK, BS_EV_CLICK), BS_ACT_STOP_RECORDING, "RECORDING OK stops");
    CHECK_STATE(&s, BS_TRANSCRIBE, "stop lands in TRANSCRIBE");
    CHECK(!s.recording, "recording flag cleared on stop");

    setup_home(&s);
    s.state = BS_RECORDING;
    s.recording = true;
    CHECK_ACT(bs_key(&s, BS_KEY_OK, BS_EV_LONG), BS_ACT_CANCEL_RECORDING,
              "RECORDING long-OK discards (safer than sending)");
    CHECK_STATE(&s, BS_HOME, "cancel lands in HOME");
    CHECK(!s.recording, "recording flag cleared on cancel");

    // UP and DOWN must not do anything while capturing.
    setup_home(&s);
    s.state = BS_RECORDING;
    CHECK_ACT(bs_key(&s, BS_KEY_UP, BS_EV_CLICK), BS_ACT_NONE, "RECORDING ignores UP");
    CHECK_ACT(bs_key(&s, BS_KEY_DOWN, BS_EV_CLICK), BS_ACT_NONE, "RECORDING ignores DOWN");
    CHECK_ACT(bs_key(&s, BS_KEY_DOWN, BS_EV_LONG), BS_ACT_NONE, "RECORDING ignores long-DOWN");
}

// ── TRANSCRIBE ──────────────────────────────────────────────────────────────

static void test_transcribe(void)
{
    bs_t s;

    setup_home(&s);
    s.state = BS_TRANSCRIBE;
    s.transcript_ready = true;
    CHECK_ACT(bs_key(&s, BS_KEY_OK, BS_EV_CLICK), BS_ACT_SEND_TRANSCRIPT, "TRANSCRIBE OK sends");
    CHECK_STATE(&s, BS_HOME, "send lands in HOME");
    CHECK(!s.transcript_ready, "transcript flag cleared on send");

    setup_home(&s);
    s.state = BS_TRANSCRIBE;
    CHECK_ACT(bs_key(&s, BS_KEY_UP, BS_EV_CLICK), BS_ACT_RERECORD, "TRANSCRIBE UP re-records");
    CHECK_STATE(&s, BS_RECORDING, "re-record lands in RECORDING");

    setup_home(&s);
    s.state = BS_TRANSCRIBE;
    CHECK_ACT(bs_key(&s, BS_KEY_DOWN, BS_EV_CLICK), BS_ACT_PAGE_TRANSCRIPT, "TRANSCRIBE DOWN pages");

    // Long press must not send.
    setup_home(&s);
    s.state = BS_TRANSCRIBE;
    CHECK_ACT(bs_key(&s, BS_KEY_OK, BS_EV_LONG), BS_ACT_RERECORD,
              "TRANSCRIBE long-OK re-records (safer than sending)");
    CHECK_STATE(&s, BS_RECORDING, "long-OK lands in RECORDING");
}

// ── DECISION: takes over, then returns ──────────────────────────────────────

static void test_decision(void)
{
    bs_t s;

    // From SUMMARY, an approval arrives and takes over.
    setup_home(&s);
    s.state = BS_SUMMARY;
    bs_notify(&s, BS_NOTIFY_DECISION);
    CHECK_STATE(&s, BS_DECISION, "decision takes over the screen");
    CHECK(s.decision_pending, "decision flag set");
    CHECK(s.return_state == BS_SUMMARY, "return state remembered");

    CHECK_ACT(bs_key(&s, BS_KEY_OK, BS_EV_CLICK), BS_ACT_CONFIRM, "DECISION OK confirms");
    CHECK_STATE(&s, BS_SUMMARY, "confirm returns to where it interrupted");
    CHECK(!s.decision_pending, "decision flag cleared");

    // From RECORDING it must not destroy the recording state machine.
    setup_home(&s);
    s.state = BS_RECORDING;
    s.recording = true;
    bs_notify(&s, BS_NOTIFY_DECISION);
    CHECK_STATE(&s, BS_DECISION, "decision interrupts recording");
    CHECK_ACT(bs_key(&s, BS_KEY_OK, BS_EV_LONG), BS_ACT_DENY,
              "DECISION long-OK denies (safer than confirming)");
    CHECK_STATE(&s, BS_RECORDING, "deny returns to RECORDING");

    // Non-OK keys do nothing while an approval is on screen.
    setup_home(&s);
    s.state = BS_SUMMARY;
    bs_notify(&s, BS_NOTIFY_DECISION);
    CHECK_ACT(bs_key(&s, BS_KEY_UP, BS_EV_CLICK), BS_ACT_NONE, "DECISION ignores UP");
    CHECK_ACT(bs_key(&s, BS_KEY_DOWN, BS_EV_CLICK), BS_ACT_NONE, "DECISION ignores DOWN");
    CHECK_STATE(&s, BS_DECISION, "still in DECISION");

    // Answered from another chat: the badge drops back without a keypress.
    bs_notify(&s, BS_NOTIFY_DECISION_DONE);
    CHECK_STATE(&s, BS_SUMMARY, "external answer returns to the prior state");
    CHECK(!s.decision_pending, "decision flag cleared externally");
}

// ── MENU ────────────────────────────────────────────────────────────────────

static void test_menu(void)
{
    bs_t s;

    setup_home(&s);
    s.state = BS_MENU;
    s.menu_count = 8;
    s.menu_index = 3;

    CHECK_ACT(bs_key(&s, BS_KEY_DOWN, BS_EV_CLICK), BS_ACT_MENU_NEXT, "MENU DOWN moves to the next row");
    CHECK(s.menu_index == 4, "menu cursor advanced");

    CHECK_ACT(bs_key(&s, BS_KEY_UP, BS_EV_CLICK), BS_ACT_MENU_PREV, "MENU UP moves to the previous row");
    CHECK(s.menu_index == 3, "menu cursor moved back");

    CHECK_ACT(bs_key(&s, BS_KEY_OK, BS_EV_CLICK), BS_ACT_MENU_STEP, "MENU OK steps the value");

    // Boundaries.
    s.menu_index = 0;
    CHECK_ACT(bs_key(&s, BS_KEY_UP, BS_EV_CLICK), BS_ACT_MENU_PREV, "MENU UP is safe at the top");
    CHECK(s.menu_index == 0, "menu cursor clamped at the top");

    s.menu_index = s.menu_count - 1;
    CHECK_ACT(bs_key(&s, BS_KEY_DOWN, BS_EV_CLICK), BS_ACT_MENU_NEXT, "MENU DOWN is safe at the bottom");
    CHECK(s.menu_index == s.menu_count - 1, "menu cursor clamped at the bottom");

    // Long press exits.
    s.state = BS_MENU;
    CHECK_ACT(bs_key(&s, BS_KEY_OK, BS_EV_LONG), BS_ACT_MENU_EXIT, "MENU long-OK exits");
    CHECK_STATE(&s, BS_HOME, "exit lands in HOME");

    s.state = BS_MENU;
    CHECK_ACT(bs_key(&s, BS_KEY_UP, BS_EV_LONG), BS_ACT_MENU_EXIT, "MENU long-UP exits");
    CHECK_STATE(&s, BS_HOME, "long-UP exit lands in HOME");
}

// ── SLEEP: the wake press does nothing else ─────────────────────────────────

static void test_sleep(void)
{
    bs_t s;
    setup_home(&s);
    s.state = BS_SUMMARY;

    CHECK_ACT(bs_notify(&s, BS_NOTIFY_IDLE), BS_ACT_SLEEP, "idle puts the badge to sleep");
    CHECK_STATE(&s, BS_SLEEP, "idle lands in SLEEP");

    // Every key must wake and nothing else.
    bs_key_t keys[3] = {BS_KEY_UP, BS_KEY_DOWN, BS_KEY_OK};
    for (int i = 0; i < 3; i++) {
        setup_home(&s);
        s.state = BS_SUMMARY;
        bs_notify(&s, BS_NOTIFY_IDLE);
        bs_action_t act = bs_key(&s, keys[i], BS_EV_CLICK);
        CHECK_ACT(act, BS_ACT_WAKE, "wake press returns WAKE only");
        CHECK_STATE(&s, BS_SUMMARY, "wake returns to the previous state");

        setup_home(&s);
        s.state = BS_SUMMARY;
        bs_notify(&s, BS_NOTIFY_IDLE);
        act = bs_key(&s, keys[i], BS_EV_LONG);
        CHECK_ACT(act, BS_ACT_WAKE, "wake long-press returns WAKE only");
        CHECK_STATE(&s, BS_SUMMARY, "wake returns to the previous state");
    }
}

// ── Link loss resets everything ─────────────────────────────────────────────

static void test_link_down(void)
{
    bs_t s;
    setup_home(&s);
    s.state = BS_RECORDING;
    s.recording = true;
    s.transcript_ready = true;
    s.decision_pending = true;

    bs_notify(&s, BS_NOTIFY_LINK_DOWN);
    CHECK_STATE(&s, BS_PAIRING, "link loss returns to PAIRING");
    CHECK(!s.recording && !s.transcript_ready && !s.decision_pending && !s.has_summary,
          "link loss clears every flag");
}

// ── A new summary pulls the wearer to the newest thing ──────────────────────

static void test_summary_notify(void)
{
    bs_t s;

    setup_home(&s);
    s.state = BS_HISTORY;
    s.history_index = 2;
    bs_notify(&s, BS_NOTIFY_SUMMARY);
    CHECK_STATE(&s, BS_SUMMARY, "new summary moves to SUMMARY");
    CHECK(s.history_index == 0, "cursor reset to the newest");

    // Recording must not be interrupted by a reply.
    setup_home(&s);
    s.state = BS_RECORDING;
    s.recording = true;
    bs_notify(&s, BS_NOTIFY_SUMMARY);
    CHECK_STATE(&s, BS_RECORDING, "a reply does not interrupt recording");
    CHECK(s.recording, "recording continues");
}

int main(void)
{
    test_boot();
    test_home();
    test_summary();
    test_history();
    test_recording();
    test_transcribe();
    test_decision();
    test_menu();
    test_sleep();
    test_link_down();
    test_summary_notify();

    if (failures) {
        fprintf(stderr, "buddy_state: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("buddy_state: all %d checks passed\n", checks);
    return 0;
}
