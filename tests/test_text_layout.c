// tests/test_text_layout.c — host tests for main/text_layout.c
//
// The cases below are the three real bugs this module exists to prevent:
//   1. "every line ends in an ellipsis"  -> wrapping must break at characters,
//      never truncate-and-ellipsise.
//   2. "the second line holds one character" -> width must be measured over whole
//      characters, not `end - line + 1` bytes.
//   3. "a comma starts the line" -> kinsoku: closing punctuation must not begin a
//      line; the previous glyph travels down with it.
//
// Build (see tools/validate.sh):
//   cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_text_layout.c main/text_layout.c

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "text_layout.h"

static int failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, (msg));  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void line_text(const char *text, const tl_line_t *l, char *out, size_t cap)
{
    size_t n = l->end - l->start;
    if (n >= cap) n = cap - 1;
    memcpy(out, text + l->start, n);
    out[n] = '\0';
}

// ── UTF-8 ───────────────────────────────────────────────────────────────────

static void test_utf8(void)
{
    uint32_t cp = 0;

    CHECK(tl_utf8_decode("A", 1, &cp) == 1 && cp == 'A', "ASCII decodes to 1 byte");
    CHECK(tl_utf8_decode("\xE4\xB8\xAD", 3, &cp) == 3 && cp == 0x4E2D, "中 is U+4E2D");
    CHECK(tl_utf8_decode("\xF0\x9F\x98\x80", 4, &cp) == 4 && cp == 0x1F600, "4-byte emoji");

    // Truncated sequence: must not consume past the buffer.
    CHECK(tl_utf8_decode("\xE4\xB8", 2, &cp) == 0, "truncated 3-byte rejected");

    // Lone continuation byte.
    CHECK(tl_utf8_decode("\x80", 1, &cp) == 0, "lone continuation rejected");

    // Overlong encoding of '/'.
    CHECK(tl_utf8_decode("\xC0\xAF", 2, &cp) == 0, "overlong form rejected");

    // UTF-16 surrogate half must never decode.
    CHECK(tl_utf8_decode("\xED\xA0\x80", 3, &cp) == 0, "surrogate rejected");

    // Empty input.
    CHECK(tl_utf8_decode("", 0, &cp) == 0, "empty input rejected");
}

// ── Width ───────────────────────────────────────────────────────────────────

static void test_units(void)
{
    CHECK(tl_cp_units('A') == 1, "ASCII is one unit");
    CHECK(tl_cp_units(' ') == 1, "space is one unit");
    CHECK(tl_cp_units(0x4E2D) == 2, "CJK ideograph is two units");
    CHECK(tl_cp_units(0xFF0C) == 2, "fullwidth comma is two units");
    CHECK(tl_cp_units(0x3042) == 2, "hiragana is two units");
}

// ── Bug 2: whole-character width ────────────────────────────────────────────

static void test_whole_character_width(void)
{
    // 10 units per line = exactly 5 CJK characters. The wrap must never split a
    // 3-byte glyph across lines: every line boundary is a valid UTF-8 boundary.
    const char *text = "明天多云转晴，东风三级";
    tl_line_t lines[16];
    int n = tl_wrap(text, strlen(text), 10, lines, 16);

    CHECK(n == 3, "10 CJK chars at 5/line gives 3 lines");

    char buf[64];
    for (int i = 0; i < n; i++) {
        line_text(text, &lines[i], buf, sizeof buf);
        // A valid UTF-8 boundary: decoding from the line start stays aligned.
        size_t p = 0;
        size_t len = strlen(buf);
        while (p < len) {
            uint32_t cp;
            size_t adv = tl_utf8_decode(buf + p, len - p, &cp);
            CHECK(adv > 0, "line content is aligned UTF-8 (no half glyph)");
            if (adv == 0) break;
            p += adv;
        }
        CHECK(lines[i].units <= 10, "line never exceeds the width budget");
    }

    // Every original character appears exactly once, in order.
    char joined[128] = {0};
    for (int i = 0; i < n; i++) {
        line_text(text, &lines[i], buf, sizeof buf);
        strcat(joined, buf);
    }
    CHECK(strcmp(joined, text) == 0, "no character lost or duplicated across lines");
}

// ── Bug 1: break at characters, never truncate ──────────────────────────────

static void test_no_truncation(void)
{
    // A line that is pure CJK with no spaces at all: the wrap must still produce
    // readable text rather than cutting and adding an ellipsis.
    const char *text = "这是一段完全没有空格的中文文本用来验证按字符断行";
    tl_line_t lines[32];
    int n = tl_wrap(text, strlen(text), 12, lines, 32);

    CHECK(n > 1, "long CJK text wraps to several lines");

    char buf[64];
    char joined[256] = {0};
    for (int i = 0; i < n; i++) {
        line_text(text, &lines[i], buf, sizeof buf);
        CHECK(strstr(buf, "...") == NULL, "wrap never invents an ellipsis");
        CHECK(strstr(buf, "…") == NULL, "wrap never invents an ellipsis (U+2026)");
        strcat(joined, buf);
    }
    CHECK(strcmp(joined, text) == 0, "wrap is lossless");
}

// ── Bug 3: kinsoku ──────────────────────────────────────────────────────────

static void test_kinsoku_line_head(void)
{
    // "，" lands exactly on a break boundary at 5 CJK chars per line. Without
    // kinsoku the second line would start with the comma.
    const char *text = "一二三四五，六七八九十";
    tl_line_t lines[16];
    int n = tl_wrap(text, strlen(text), 10, lines, 16);

    CHECK(n >= 2, "text wraps");

    char buf[64];
    for (int i = 0; i < n; i++) {
        line_text(text, &lines[i], buf, sizeof buf);
        uint32_t first = 0;
        tl_utf8_decode(buf, strlen(buf), &first);
        CHECK(first != 0xFF0C, "line must not start with a fullwidth comma");
        CHECK(first != 0x3002, "line must not start with a full stop");
    }

    // The comma must stay attached to the glyph before it.
    for (int i = 0; i < n; i++) {
        line_text(text, &lines[i], buf, sizeof buf);
        CHECK(strstr(buf, "五，") != NULL || strstr(buf, "，") == NULL,
              "comma travels with the preceding glyph");
    }
}

static void test_kinsoku_line_tail(void)
{
    // An opening bracket must not end a line.
    const char *text = "一二三四五六（七八九十）";
    tl_line_t lines[16];
    int n = tl_wrap(text, strlen(text), 12, lines, 16);

    char buf[64];
    for (int i = 0; i < n; i++) {
        line_text(text, &lines[i], buf, sizeof buf);
        size_t len = strlen(buf);
        size_t p = 0;
        uint32_t cp = 0;
        while (p < len) { p += tl_utf8_decode(buf + p, len - p, &cp); }
        CHECK(!tl_is_line_tail_forbidden(cp), "line must not end with an opening bracket");
    }
    CHECK(n >= 1, "bracket text wraps");
}

// ── Forced newlines ─────────────────────────────────────────────────────────

static void test_explicit_newline(void)
{
    const char *text = "第一行\n第二行";
    tl_line_t lines[8];
    int n = tl_wrap(text, strlen(text), 40, lines, 8);

    CHECK(n == 2, "explicit newline forces a break");

    char buf[64];
    line_text(text, &lines[0], buf, sizeof buf);
    CHECK(strcmp(buf, "第一行") == 0, "first line excludes the newline");
    line_text(text, &lines[1], buf, sizeof buf);
    CHECK(strcmp(buf, "第二行") == 0, "second line excludes the newline");
}

// ── Degenerate input ────────────────────────────────────────────────────────

static void test_edge_cases(void)
{
    tl_line_t lines[8];

    CHECK(tl_wrap("", 0, 10, lines, 8) == 0, "empty text gives zero lines");
    CHECK(tl_wrap(NULL, 0, 10, lines, 8) == 0, "NULL text is safe");
    CHECK(tl_wrap("abc", 3, 10, NULL, 8) == 0, "NULL lines is safe");
    CHECK(tl_wrap("abc", 3, 0, lines, 8) == 0, "zero width is safe");
    CHECK(tl_wrap("abc", 3, 10, lines, 0) == 0, "zero capacity is safe");

    // One character wider than the whole budget must still make progress.
    int n = tl_wrap("中", 3, 1, lines, 8);
    CHECK(n >= 1, "over-wide character still produces a line (no infinite loop)");

    // A single line that fits exactly.
    n = tl_wrap("中文字", 9, 6, lines, 8);
    CHECK(n == 1, "exactly-fitting text is one line");
}

// ── Paging ──────────────────────────────────────────────────────────────────

static void test_paging(void)
{
    // 15 CJK chars at 4/line = 4 lines; 2 lines per page = 2 pages.
    const char *text = "一二三四五六七八九十一二三四五";

    int pages = tl_page_count(text, strlen(text), 8, 2);
    CHECK(pages == 2, "15 chars at 4/line, 2 lines/page = 2 pages");

    char page0[128] = {0}, page1[128] = {0};
    tl_page_text(text, strlen(text), 8, 2, 0, page0, sizeof page0);
    tl_page_text(text, strlen(text), 8, 2, 1, page1, sizeof page1);

    // Concatenating the pages (minus inserted newlines) must give back the text.
    char joined[256] = {0};
    strcat(joined, page0);
    // page0's internal newlines are separators between its two lines; strip them.
    char flat[256] = {0};
    size_t k = 0;
    for (const char *p = page0; *p; p++) if (*p != '\n') flat[k++] = *p;
    for (const char *p = page1; *p; p++) if (*p != '\n') flat[k++] = *p;
    flat[k] = '\0';

    CHECK(strcmp(flat, text) == 0, "pages concatenate back to the original text");
    (void)joined;

    // Out-of-range page is clamped, not an overrun.
    char out[128];
    size_t w = tl_page_text(text, strlen(text), 8, 2, 99, out, sizeof out);
    CHECK(w > 0, "out-of-range page clamps to the last page");
    CHECK(tl_page_text(text, strlen(text), 8, 2, -1, out, sizeof out) > 0,
          "negative page clamps to the first page");
}

// ── Buffer safety ───────────────────────────────────────────────────────────

static void test_output_truncation(void)
{
    const char *text = "一二三四五六七八九十";
    char small[8];

    // Must truncate on a character boundary and always NUL-terminate.
    size_t w = tl_page_text(text, strlen(text), 4, 2, 0, small, sizeof small);
    CHECK(w < sizeof small, "output respects the capacity");
    CHECK(small[w] == '\0', "output is NUL-terminated");

    size_t p = 0;
    while (p < w) {
        uint32_t cp;
        size_t adv = tl_utf8_decode(small + p, w - p, &cp);
        CHECK(adv > 0, "truncated output stays on character boundaries");
        if (adv == 0) break;
        p += adv;
    }
}

int main(void)
{
    test_utf8();
    test_units();
    test_whole_character_width();
    test_no_truncation();
    test_kinsoku_line_head();
    test_kinsoku_line_tail();
    test_explicit_newline();
    test_edge_cases();
    test_paging();
    test_output_truncation();

    if (failures) {
        fprintf(stderr, "text_layout: %d failure(s)\n", failures);
        return 1;
    }
    printf("text_layout: all tests passed\n");
    return 0;
}
