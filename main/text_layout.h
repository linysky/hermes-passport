// main/text_layout.h — CJK-aware text layout for the 240x320 badge screen.
//
// Pure C11, no ESP-IDF and no LVGL: everything here is a calculation and must be
// covered by host tests (see tests/test_text_layout.c).
//
// Why this exists instead of letting the renderer wrap:
//   1. CJK has no spaces. A "wrap at whitespace" strategy truncates every full line.
//   2. Width must be measured over WHOLE characters. Measuring `end - line + 1`
//      bytes lands in the middle of a 3-byte glyph, so every line loses one
//      character and that character is pushed onto a line of its own.
//   3. Kinsoku: a line must not START with closing punctuation. The fix is to move
//      the current line's last character down with it — never to over-fill the line,
//      because the renderer clips over-wide content and the punctuation would
//      disappear together with the previous glyph.
//
// Widths are counted in half-width units: ASCII = 1, CJK/full-width = 2. The caller
// converts a pixel width into units using the active font.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Measured on this panel with the 16 px CJK font: the baseline advance is 21 px,
// NOT 10 px. Scroll limits computed from a wrong line height come out half as tall
// as the real content and the last lines become unreachable.
#define TL_LINE_HEIGHT_PX 21

// The badge body budget. One summary must fit a single screen:
// ~170 CJK characters. Raising this past ~3 KB together with the history ring has
// been observed to make the C3 silently stop advertising BLE (no PSRAM).
#define TL_SUMMARY_MAX_BYTES 512
#define TL_WRAP_OUT_BYTES 768

typedef struct {
    size_t start;   // byte offset of the first byte of this line
    size_t end;     // byte offset one past the last byte of this line
    int units;      // half-width units consumed by this line
} tl_line_t;

// Decode one UTF-8 codepoint. Returns the number of bytes consumed (1..4), or 0 on
// an invalid sequence (the caller should then step one byte and resynchronise).
size_t tl_utf8_decode(const char *s, size_t len, uint32_t *out_cp);

// Half-width units occupied by a codepoint: 2 for CJK/full-width, 1 otherwise.
int tl_cp_units(uint32_t cp);

// True when cp may not begin a line (closing punctuation, trailing small kana, ...).
bool tl_is_line_head_forbidden(uint32_t cp);

// True when cp may not end a line (opening brackets and quotes).
bool tl_is_line_tail_forbidden(uint32_t cp);

// Wrap `text` into lines of at most `max_units` half-width units each.
// Writes at most `max_lines` entries into `lines` and returns the number of lines
// produced. A line that would exceed max_lines is dropped (the caller shows a
// page counter instead of silently losing text).
//
// The function applies kinsoku: when a break would land on a line-head-forbidden
// character, the break moves back one character so that character travels with the
// preceding glyph onto the next line.
int tl_wrap(const char *text, size_t len, int max_units, tl_line_t *lines, int max_lines);

// Page a long text into screens of `max_lines` lines each.
// Returns the number of pages. `page` is clamped into range.
int tl_page_count(const char *text, size_t len, int max_units, int max_lines);

// Fill `out` (capacity `out_cap`) with the bytes of one page, NUL-terminated.
// Returns the number of bytes written, excluding the NUL. Truncation happens on a
// character boundary — never in the middle of a glyph.
size_t tl_page_text(const char *text, size_t len, int max_units, int max_lines,
                    int page, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
