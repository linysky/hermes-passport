// main/history_ring.h — 10-slot ring buffer of reply summaries.
//
// Pure C11, no ESP-IDF, no LVGL, no heap: the storage belongs to the caller so the
// module can be dropped into a host test unchanged. See tests/test_history_ring.c.
//
// Why a ring and not a list:
//   The badge is RAM-tight (no PSRAM) and BLE replay is the recovery path for a
//   reboot. The device keeps the last N summaries in RAM only; the bridge replays
//   them on every reconnect, so a reboot costs nothing but a reconnect.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One summary must fit a screen: ~170 CJK characters. Raising this past ~3 KB
// together with LVGL's canvas has been observed to make the C3 silently stop
// advertising BLE. See text_layout.h.
#define HR_SLOT_BYTES 512
#define HR_SLOTS 10

typedef struct {
    char text[HR_SLOT_BYTES];   // NUL-terminated summary
    uint32_t ts;                // unix seconds, for display only (0 = unknown)
    uint32_t id;                // monotonic id used for dedup
} hr_slot_t;

typedef struct {
    hr_slot_t slots[HR_SLOTS];
    int count;                  // how many slots are in use (<= HR_SLOTS)
    int newest;                 // index of the newest slot, -1 when empty
    uint32_t last_id;           // id of the newest entry, for dedup
} hr_ring_t;

void hr_init(hr_ring_t *r);

// Push a summary. `id` is the bridge's message id; a repeat of the same id is
// ignored and returns false, so a reconnect replay never duplicates a slot.
// `text` is truncated to HR_SLOT_BYTES-1 on a UTF-8 character boundary.
bool hr_push(hr_ring_t *r, uint32_t id, const char *text, uint32_t ts);

// Index 0 is the newest. Returns false when `index` is out of range.
bool hr_get(const hr_ring_t *r, int index, hr_slot_t *out);

// Number of stored slots (0..HR_SLOTS).
int hr_count(const hr_ring_t *r);

// Find a slot by id. Returns -1 when not found.
int hr_find_id(const hr_ring_t *r, uint32_t id);

// Truncate `src` into `dst` (capacity `cap`) on a UTF-8 character boundary.
// Returns the number of bytes written, excluding the NUL. Always NUL-terminates
// when cap > 0. Never cuts a multi-byte glyph in half.
size_t hr_truncate_utf8(char *dst, size_t cap, const char *src);

#ifdef __cplusplus
}
#endif
