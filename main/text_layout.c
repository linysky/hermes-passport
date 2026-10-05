// main/text_layout.c — CJK-aware text layout. See text_layout.h for the rationale.
//
// Pure C11. No ESP-IDF, no LVGL, no heap. Every function here is a calculation and
// is covered by tests/test_text_layout.c.

#include "text_layout.h"

// ── UTF-8 ───────────────────────────────────────────────────────────────────

size_t tl_utf8_decode(const char *s, size_t len, uint32_t *out_cp)
{
    if (!s || len == 0) return 0;
    const unsigned char *p = (const unsigned char *)s;
    unsigned char b0 = p[0];

    if (b0 < 0x80) {
        if (out_cp) *out_cp = b0;
        return 1;
    }
    size_t need;
    uint32_t cp;
    if ((b0 & 0xE0) == 0xC0) { need = 2; cp = b0 & 0x1Fu; }
    else if ((b0 & 0xF0) == 0xE0) { need = 3; cp = b0 & 0x0Fu; }
    else if ((b0 & 0xF8) == 0xF0) { need = 4; cp = b0 & 0x07u; }
    else return 0;                       // continuation byte or invalid lead

    if (len < need) return 0;
    for (size_t i = 1; i < need; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;   // bad continuation
        cp = (cp << 6) | (uint32_t)(p[i] & 0x3Fu);
    }
    // Reject overlong forms and surrogates so callers never see a bogus width.
    if (need == 2 && cp < 0x80u) return 0;
    if (need == 3 && cp < 0x800u) return 0;
    if (need == 4 && cp < 0x10000u) return 0;
    if (cp >= 0xD800u && cp <= 0xDFFFu) return 0;
    if (cp > 0x10FFFFu) return 0;

    if (out_cp) *out_cp = cp;
    return need;
}

// ── Width ───────────────────────────────────────────────────────────────────

int tl_cp_units(uint32_t cp)
{
    // CJK and full-width ranges occupy two half-width units. Everything else
    // (ASCII, Latin-1, Cyrillic, ...) is one.
    if (cp < 0x1100u) return 1;
    // Hangul Jamo
    if (cp >= 0x1100u && cp <= 0x115Fu) return 2;
    // CJK punctuation, kana, CJK unified ideographs, full-width forms
    if (cp >= 0x2E80u && cp <= 0x303Eu) return 2;
    if (cp >= 0x3041u && cp <= 0x33FFu) return 2;
    if (cp >= 0x3400u && cp <= 0x4DBFu) return 2;
    if (cp >= 0x4E00u && cp <= 0x9FFFu) return 2;
    if (cp >= 0xA000u && cp <= 0xA4CFu) return 2;
    if (cp >= 0xAC00u && cp <= 0xD7A3u) return 2;   // Hangul syllables
    if (cp >= 0xF900u && cp <= 0xFAFFu) return 2;
    if (cp >= 0xFE30u && cp <= 0xFE4Fu) return 2;   // CJK compatibility forms
    if (cp >= 0xFF00u && cp <= 0xFF60u) return 2;   // full-width forms
    if (cp >= 0xFFE0u && cp <= 0xFFE6u) return 2;
    if (cp >= 0x20000u && cp <= 0x2FFFDu) return 2; // CJK extension B+
    return 1;
}

// ── Kinsoku (line-head / line-tail forbidden characters) ────────────────────

bool tl_is_line_head_forbidden(uint32_t cp)
{
    // Must not START a line: closing punctuation, trailing small kana, and
    // the ellipsis/leader marks that read as continuation.
    switch (cp) {
        case 0xFF0C:  // ，FULLWIDTH COMMA
        case 0x3002:  // 。 IDEOGRAPHIC FULL STOP
        case 0x3001:  // 、 IDEOGRAPHIC COMMA
        case 0xFF1B:  // ；
        case 0xFF1A:  // ：
        case 0xFF1F:  // ？
        case 0xFF01:  // ！
        case 0xFF09:  // ）
        case 0x3009:  // 》
        case 0x300B:  // 》》
        case 0x300D:  // 」
        case 0x300F:  // 』
        case 0x3011:  // 】
        case 0x3015:  // 〕
        case 0x3017:  // 〕
        case 0x3019:  // 〕
        case 0x301B:  // 】
        case 0x301F:  // 】
        case 0x2026:  // … HORIZONTAL ELLIPSIS
        case 0x2025:  // ‥ TWO DOT LEADER
        case 0x00B7:  // · MIDDLE DOT
        case 0x30FB:  // ・ KATAKANA MIDDLE DOT
        case 0xFF0E:  // ．
        case 0xFF05:  // ％
        case 0x3005:  // 々
        case 0x3006:  // 〆
        case 0x3007:  // 〇
        // Closing quotes
        case 0x201D:  // ” RIGHT DOUBLE QUOTE
        case 0x2019:  // ’ RIGHT SINGLE QUOTE
        case 0x300C:  // 「 (used as closing in some layouts)
        case 0x300E:  // 『
        // Trailing small kana
        case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049:
        case 0x3063: case 0x3083: case 0x3085: case 0x3087: case 0x308E:
        case 0x3095: case 0x3096:
        case 0x30A1: case 0x30A3: case 0x30A5: case 0x30A7: case 0x30A9:
        case 0x30C3: case 0x30E3: case 0x30E5: case 0x30E7: case 0x30EE:
        case 0x30F5: case 0x30F6:
            return true;
        default:
            return false;
    }
}

bool tl_is_line_tail_forbidden(uint32_t cp)
{
    // Must not END a line: opening brackets and quotes.
    switch (cp) {
        case 0xFF08:  // （ FULLWIDTH LEFT PARENTHESIS
        case 0x3008:  // 〈
        case 0x300A:  // 《
        case 0x300C:  // 「
        case 0x300E:  // 『
        case 0x3010:  // 【
        case 0x3014:  // 〔
        case 0x3016:  // 〖
        case 0x3018:  // 〘
        case 0x301A:  // 〚
        case 0x201C:  // “ LEFT DOUBLE QUOTE
        case 0x2018:  // ‘ LEFT SINGLE QUOTE
        case 0xFF3B:  // ［
        case 0xFF5B:  // ｛
        case 0xFF5F:  // ｟
            return true;
        default:
            return false;
    }
}

// ── Core wrapping ───────────────────────────────────────────────────────────

// Decode one character, normalising invalid bytes to U+FFFD with a 1-byte step so
// the caller always makes progress and never loops.
static size_t step_char(const char *text, size_t len, size_t pos, uint32_t *cp)
{
    size_t adv = tl_utf8_decode(text + pos, len - pos, cp);
    if (adv == 0) {
        *cp = 0xFFFDu;
        return 1;
    }
    return adv;
}

// Compute the end of the line starting at `start`, and the start of the next line.
// `line_end` excludes a terminating '\n'; `next_start` skips past it.
static void line_break(const char *text, size_t len, size_t start, int max_units,
                       size_t *line_end, size_t *next_start)
{
    size_t pos = start;
    size_t prev_start = start;   // start of the previous character
    size_t prev_prev_start = start;
    bool has_prev = false;
    bool has_prev2 = false;
    int units = 0;

    while (pos < len) {
        uint32_t cp;
        size_t adv = step_char(text, len, pos, &cp);

        if (cp == '\n') {
            *line_end = pos;
            *next_start = pos + adv;
            return;
        }

        int u = tl_cp_units(cp);
        if (units > 0 && units + u > max_units) {
            // Break at `pos`, then apply kinsoku to the two ends of the line.
            size_t brk = pos;
            size_t nb = pos;      // next line starts at the overflowing char

            // 1) Line-head forbidden: the character starting the next line may not
            //    be closing punctuation. Move the previous character down with it,
            //    so the punctuation travels behind its own glyph.
            if (tl_is_line_head_forbidden(cp) && has_prev && prev_start > start) {
                brk = prev_start;
                nb = prev_start;
            }

            // 2) Line-tail forbidden: the character now ending this line may not be
            //    an opening bracket. Move it down as well.
            size_t last_start = (brk == pos) ? prev_start : prev_prev_start;
            bool have_last = (brk == pos) ? has_prev : has_prev2;
            if (have_last && last_start > start) {
                uint32_t tail_cp = 0;
                (void)step_char(text, len, last_start, &tail_cp);
                if (tl_is_line_tail_forbidden(tail_cp)) {
                    size_t back = start, p = start;
                    uint32_t tmp;
                    while (p < last_start) { back = p; p += step_char(text, len, p, &tmp); }
                    if (back > start) {
                        brk = back;
                        nb = back;
                    }
                }
            }

            // Never return an empty line: that would make the caller loop forever.
            if (brk <= start) { brk = pos; nb = pos; }

            *line_end = brk;
            *next_start = nb;
            return;
        }

        prev_prev_start = prev_start;
        has_prev2 = has_prev;
        prev_start = pos;
        has_prev = true;

        units += u;
        pos += adv;
    }

    *line_end = len;
    *next_start = len;
}

int tl_wrap(const char *text, size_t len, int max_units, tl_line_t *lines, int max_lines)
{
    if (!text || !lines || max_lines <= 0 || max_units <= 0) return 0;

    int n = 0;
    size_t start = 0;
    while (start < len && n < max_lines) {
        size_t end, next;
        line_break(text, len, start, max_units, &end, &next);
        if (end > start || next > start) {
            lines[n].start = start;
            lines[n].end = end;
            lines[n].units = 0;
            // Recompute the line's width so callers can right-align or centre.
            int units = 0;
            size_t p = start;
            uint32_t cp;
            while (p < end) { p += step_char(text, len, p, &cp); units += tl_cp_units(cp); }
            lines[n].units = units;
            n++;
        }
        if (next <= start) break;      // safety: always make progress
        start = next;
    }
    return n;
}

// ── Paging ──────────────────────────────────────────────────────────────────

int tl_page_count(const char *text, size_t len, int max_units, int max_lines)
{
    if (!text || max_lines <= 0 || max_units <= 0) return 1;

    int total = 0;
    size_t start = 0;
    while (start < len) {
        size_t end, next;
        line_break(text, len, start, max_units, &end, &next);
        total++;
        if (next <= start) break;
        start = next;
    }
    if (total == 0) total = 1;
    return (total + max_lines - 1) / max_lines;
}

size_t tl_page_text(const char *text, size_t len, int max_units, int max_lines,
                    int page, char *out, size_t out_cap)
{
    if (!text || !out || out_cap == 0 || max_lines <= 0 || max_units <= 0) {
        if (out && out_cap) out[0] = '\0';
        return 0;
    }

    int pages = tl_page_count(text, len, max_units, max_lines);
    if (page < 0) page = 0;
    if (page >= pages) page = pages - 1;
    int skip = page * max_lines;

    size_t written = 0;
    size_t start = 0;
    int line_no = 0;
    while (start < len) {
        size_t end, next;
        line_break(text, len, start, max_units, &end, &next);
        if (line_no >= skip && line_no < skip + max_lines) {
            if (written > 0 && written + 1 < out_cap) {
                out[written++] = '\n';
            }
            for (size_t p = start; p < end && written + 1 < out_cap; p++) {
                out[written++] = text[p];
            }
        }
        line_no++;
        if (next <= start) break;
        start = next;
        if (line_no >= skip + max_lines) break;
    }

    out[written] = '\0';
    return written;
}
