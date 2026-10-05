// tests/test_history_ring.c — host tests for main/history_ring.c
//
// Build (see tools/validate.sh):
//   cc -std=c11 -Wall -Wextra -Werror -Imain
//      tests/test_history_ring.c main/history_ring.c main/text_layout.c

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "history_ring.h"
#include "text_layout.h"   // tl_utf8_decode: verify truncation lands on boundaries

static int failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, (msg));  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void test_empty(void)
{
    hr_ring_t r;
    hr_init(&r);

    CHECK(hr_count(&r) == 0, "fresh ring is empty");
    CHECK(!hr_get(&r, 0, NULL), "get on empty ring fails");
    CHECK(hr_find_id(&r, 1) == -1, "find on empty ring returns -1");

    hr_slot_t s;
    CHECK(!hr_get(&r, 0, &s), "index 0 on empty ring fails");
}

static void test_order(void)
{
    hr_ring_t r;
    hr_init(&r);

    CHECK(hr_push(&r, 1, "第一条", 100), "push 1");
    CHECK(hr_push(&r, 2, "第二条", 200), "push 2");
    CHECK(hr_push(&r, 3, "第三条", 300), "push 3");

    CHECK(hr_count(&r) == 3, "three slots in use");

    hr_slot_t s;
    CHECK(hr_get(&r, 0, &s) && s.id == 3, "index 0 is the newest");
    CHECK(hr_get(&r, 1, &s) && s.id == 2, "index 1 is the second newest");
    CHECK(hr_get(&r, 2, &s) && s.id == 1, "index 2 is the oldest");
    CHECK(!hr_get(&r, 3, &s), "index 3 is out of range");

    CHECK(hr_get(&r, 0, &s) && strcmp(s.text, "第三条") == 0, "text is preserved");
    CHECK(s.ts == 300, "timestamp is preserved");
}

// The reconnect replay must not create duplicate slots for the same message.
static void test_dedup(void)
{
    hr_ring_t r;
    hr_init(&r);

    CHECK(hr_push(&r, 7, "hello", 1), "first push accepted");
    CHECK(!hr_push(&r, 7, "hello", 1), "same id rejected");
    CHECK(!hr_push(&r, 7, "hello again", 2), "same id rejected even with new text");

    CHECK(hr_count(&r) == 1, "dedup keeps exactly one slot");

    // A different id is fine.
    CHECK(hr_push(&r, 8, "world", 2), "different id accepted");
    CHECK(hr_count(&r) == 2, "two distinct ids occupy two slots");
}

// Overflow past HR_SLOTS must evict the oldest, keeping the newest.
static void test_overflow(void)
{
    hr_ring_t r;
    hr_init(&r);

    char text[32];
    for (int i = 0; i < HR_SLOTS + 5; i++) {
        snprintf(text, sizeof text, "msg %d", i);
        CHECK(hr_push(&r, (uint32_t)(i + 1), text, (uint32_t)i), "push during fill");
    }

    CHECK(hr_count(&r) == HR_SLOTS, "ring never exceeds its capacity");

    hr_slot_t s;
    CHECK(hr_get(&r, 0, &s) && s.id == (uint32_t)(HR_SLOTS + 5), "newest survived");
    CHECK(hr_get(&r, HR_SLOTS - 1, &s) && s.id == 6,
          "oldest surviving is the one after the five evicted");
    CHECK(hr_find_id(&r, 1) == -1, "evicted id is gone");
}

// Truncation must never split a multi-byte glyph.
static void test_utf8_truncation(void)
{
    char buf[8];

    // 3 CJK chars = 9 bytes. Capacity 8 must hold 2 chars (6 bytes) + NUL.
    size_t n = hr_truncate_utf8(buf, sizeof buf, "中文字");
    CHECK(n == 6, "truncation lands on a character boundary");
    CHECK(strcmp(buf, "中文") == 0, "whole characters only");

    // Capacity exactly fits: 3 chars + NUL = 10 bytes.
    char exact[10];
    n = hr_truncate_utf8(exact, sizeof exact, "中文字");
    CHECK(n == 9, "exact capacity keeps every character");
    CHECK(strcmp(exact, "中文字") == 0, "no loss when it fits");

    // Mixed ASCII and CJK: cap 5 admits "ab" (2 bytes) then refuses "中"
    // (would need 2+3+1 = 6). The cut must fall between characters.
    char mixed[8];
    n = hr_truncate_utf8(mixed, sizeof mixed, "ab中c");
    CHECK(n == 6 && strcmp(mixed, "ab中c") == 0, "mixed width fits when it fits");

    char cut[5];
    n = hr_truncate_utf8(cut, sizeof cut, "ab中c");
    CHECK(n == 2 && strcmp(cut, "ab") == 0, "cut lands between characters, not inside 中");

    // Whatever is kept must be re-decodable as whole UTF-8 characters.
    size_t p = 0;
    while (p < n) {
        uint32_t cp = 0;
        size_t adv = tl_utf8_decode(cut + p, n - p, &cp);
        CHECK(adv > 0, "truncated output stays on character boundaries");
        if (adv == 0) break;
        p += adv;
    }

    // Degenerate inputs.
    CHECK(hr_truncate_utf8(buf, 0, "abc") == 0, "zero capacity is safe");
    CHECK(hr_truncate_utf8(NULL, 8, "abc") == 0, "NULL dst is safe");
    char one[1];
    CHECK(hr_truncate_utf8(one, 1, "中") == 0 && one[0] == '\0',
          "capacity 1 keeps only the NUL");
}

// A summary longer than a slot must be truncated into the slot, not overflow it.
static void test_slot_truncation(void)
{
    hr_ring_t r;
    hr_init(&r);

    char long_text[HR_SLOT_BYTES * 2];
    memset(long_text, 'A', sizeof long_text - 1);
    long_text[sizeof long_text - 1] = '\0';

    CHECK(hr_push(&r, 1, long_text, 0), "over-long summary accepted");

    hr_slot_t s;
    CHECK(hr_get(&r, 0, &s), "slot readable");
    CHECK(strlen(s.text) == HR_SLOT_BYTES - 1, "slot holds the truncated length");
    CHECK(s.text[HR_SLOT_BYTES - 1] == '\0', "slot is NUL-terminated");
}

static void test_null_safety(void)
{
    hr_init(NULL);              // must not crash
    CHECK(hr_count(NULL) == 0, "NULL ring count is 0");
    CHECK(hr_find_id(NULL, 1) == -1, "NULL ring find is -1");

    hr_ring_t r;
    hr_init(&r);
    CHECK(!hr_push(&r, 1, NULL, 0), "NULL text is rejected");
}

int main(void)
{
    test_empty();
    test_order();
    test_dedup();
    test_overflow();
    test_utf8_truncation();
    test_slot_truncation();
    test_null_safety();

    if (failures) {
        fprintf(stderr, "history_ring: %d failure(s)\n", failures);
        return 1;
    }
    printf("history_ring: all tests passed\n");
    return 0;
}
