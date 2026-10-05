// main/summary_extract.c — badge-sized summary extraction. See summary_extract.h.

#include "summary_extract.h"

#include <string.h>

#include "text_layout.h"   // tl_utf8_decode: keep truncation on character boundaries

// ── Helpers ─────────────────────────────────────────────────────────────────

// Shrink the buffer to `new_len`, always NUL-terminating.
static size_t terminate(char *text, size_t new_len)
{
    text[new_len] = '\0';
    return new_len;
}

// ── HTML comments ───────────────────────────────────────────────────────────

size_t se_strip_comments(char *text, size_t len)
{
    if (!text) return 0;

    size_t out = 0;
    size_t i = 0;

    while (i < len) {
        // A comment opener?
        if (i + 4 <= len && memcmp(text + i, "<!--", 4) == 0) {
            // Find the close. If it never closes, the malformed comment runs
            // to the end of its line and must still disappear.
            size_t j = i + 4;
            size_t close = len;
            bool closed = false;
            while (j + 3 <= len) {
                if (memcmp(text + j, "-->", 3) == 0) {
                    close = j + 3;
                    closed = true;
                    break;
                }
                j++;
            }
            if (!closed) {
                // Drop through the end of the line, not the whole rest.
                close = i + 4;
                while (close < len && text[close] != '\n') close++;
            }
            i = close;
            continue;
        }

        text[out++] = text[i++];
    }

    return terminate(text, out);
}

// ── Inline Markdown ─────────────────────────────────────────────────────────

size_t se_strip_inline(char *text, size_t len)
{
    if (!text) return 0;

    size_t out = 0;
    size_t i = 0;

    while (i < len) {
        // Fenced code block: drop the whole block, it is not a summary.
        if (i + 3 <= len && memcmp(text + i, "```", 3) == 0) {
            size_t j = i + 3;
            while (j + 3 <= len && memcmp(text + j, "```", 3) != 0) j++;
            i = (j + 3 <= len) ? j + 3 : len;
            continue;
        }

        // Inline code: keep the code, drop the markers.
        if (text[i] == '`') {
            size_t j = i + 1;
            while (j < len && text[j] != '`') j++;
            if (j < len) {
                for (size_t k = i + 1; k < j; k++) text[out++] = text[k];
                i = j + 1;
            } else {
                // Unterminated: copy the marker verbatim rather than swallow.
                text[out++] = text[i++];
            }
            continue;
        }

        // Image: ![alt](url) -> nothing.
        if (text[i] == '!' && i + 1 < len && text[i + 1] == '[') {
            size_t j = i + 2;
            while (j < len && text[j] != ']') j++;
            if (j + 1 < len && text[j + 1] == '(') {
                while (j < len && text[j] != ')') j++;
                i = (j < len) ? j + 1 : len;
                continue;
            }
        }

        // Link: [label](url) -> label.
        if (text[i] == '[') {
            size_t j = i + 1;
            while (j < len && text[j] != ']') j++;
            if (j + 1 < len && text[j + 1] == '(') {
                size_t label_start = i + 1;
                size_t label_end = j;
                while (j < len && text[j] != ')') j++;
                for (size_t k = label_start; k < label_end; k++) text[out++] = text[k];
                i = (j < len) ? j + 1 : len;
                continue;
            }
        }

        text[out++] = text[i++];
    }

    return terminate(text, out);
}

// ── Summary ─────────────────────────────────────────────────────────────────

// Copy `src[start,end)` into `out` at `out_len`, stopping at `cap-1` bytes on a
// character boundary. Returns the new out_len.
static size_t copy_bounded(char *out, size_t out_len, size_t cap,
                           const char *src, size_t start, size_t end)
{
    size_t p = start;
    while (p < end) {
        uint32_t cp = 0;
        size_t adv = tl_utf8_decode(src + p, end - p, &cp);
        if (adv == 0) { adv = 1; cp = 0xFFFDu; }
        if (out_len + adv + 1 > cap) break;   // leave room for the NUL
        memcpy(out + out_len, src + p, adv);
        out_len += adv;
        p += adv;
    }
    return out_len;
}

size_t se_summary(const char *text, size_t len, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) return 0;
    out[0] = '\0';
    if (!text || len == 0) return 0;

    // Work on a bounded scratch copy: callers pass arbitrary replies.
    enum { SCRATCH = 4096 };
    char scratch[SCRATCH];
    size_t n = len < SCRATCH - 1 ? len : SCRATCH - 1;
    memcpy(scratch, text, n);
    scratch[n] = '\0';

    n = se_strip_comments(scratch, n);
    n = se_strip_inline(scratch, n);

    size_t start = 0;
    size_t end = n;

    // Skip leading blank lines and indentation.
    while (start < n && (scratch[start] == '\n' || scratch[start] == '\r' ||
                         scratch[start] == ' ' || scratch[start] == '\t')) {
        start++;
    }

    // A `TITLE:` line anywhere in the reply wins over the first paragraph: it
    // is what the reply is about, and a long preamble would otherwise push it
    // off screen. Scanning the whole reply matters -- a title commonly follows
    // an intro, not precedes it.
    bool found_title = false;
    {
        size_t p = start;
        while (p < n) {
            size_t line_start = p;
            while (p < n && scratch[p] != '\n') p++;
            size_t line_end = p;

            size_t k = line_start;
            while (k < line_end && (scratch[k] == ' ' || scratch[k] == '\t')) k++;
            if (line_end - k >= 6 && strncmp(scratch + k, "TITLE:", 6) == 0) {
                start = k + 6;
                while (start < line_end && (scratch[start] == ' ' || scratch[start] == '\t')) start++;
                end = line_end;
                found_title = true;
                break;
            }
            if (p < n) p++;   // consume the newline
        }
    }

    if (!found_title) {
        // First paragraph: up to the first blank line.
        size_t p = start;
        end = n;
        while (p < n) {
            if (scratch[p] == '\n') {
                if (p + 1 >= n || scratch[p + 1] == '\n' || scratch[p + 1] == '\r') {
                    end = p;
                    break;
                }
            }
            p++;
        }
    }

    size_t written = copy_bounded(out, 0, out_cap, scratch, start, end);
    out[written] = '\0';   // copy_bounded stops on capacity, never terminates
    return written;
}

// ── Hallucination guard ─────────────────────────────────────────────────────

size_t se_trim_hallucination(char *text, size_t len,
                             const char *const *phrases, int phrase_count)
{
    if (!text || !phrases || phrase_count <= 0) return len;

    bool changed = true;
    while (changed) {
        changed = false;

        // Trim trailing whitespace BEFORE matching: a hallucinated phrase
        // arrives after silence, so the buffer usually ends with spaces and a
        // tail comparison would never match otherwise.
        while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' ||
                           text[len - 1] == '\n' || text[len - 1] == '\r')) {
            len--;
        }
        if (len == 0) break;

        for (int i = 0; i < phrase_count; i++) {
            const char *p = phrases[i];
            if (!p || !*p) continue;
            size_t plen = strlen(p);
            if (plen == 0 || plen > len) continue;

            // Only a tail match counts: a phrase in the middle of a real
            // sentence is left alone rather than deleting the sentence.
            if (memcmp(text + len - plen, p, plen) == 0) {
                len -= plen;
                text[len] = '\0';
                changed = true;
                break;
            }
        }
    }

    return terminate(text, len);
}
