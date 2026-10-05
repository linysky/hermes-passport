// main/summary_extract.h — turn a bot reply into a badge-sized summary.
//
// Pure C11, no ESP-IDF, no LVGL, no heap. Covered by tests/test_summary_extract.c.
//
// Why the firmware extracts its own summary instead of trusting the reply:
//   * The screen budget is one screen: ~170 CJK characters (512 bytes).
//     A reply is Markdown and can be arbitrarily long.
//   * The speak-brief block (`<!--SPK ...-->`) exists for the voice layer and
//     must never reach the screen.
//   * Whichever layer extracts it, the truncation must land on a UTF-8
//     character boundary or the renderer draws a broken glyph.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One summary must fit a screen. See text_layout.h for why this stays small.
#define SE_SUMMARY_MAX_BYTES 512

// Extract a summary from `text` into `out` (capacity `out_cap`).
//
// Pipeline: strip HTML comments (including a malformed SPK line that never
// closes), strip fenced code blocks, strip inline code markers, drop image
// syntax, reduce links to their label, then take the first paragraph or the
// `TITLE:` line if one is present. Finally truncate to SE_SUMMARY_MAX_BYTES-1
// on a character boundary.
//
// Returns the number of bytes written, excluding the NUL. Always NUL-terminates
// when out_cap > 0. Never emits a half glyph.
size_t se_summary(const char *text, size_t len, char *out, size_t out_cap);

// Strip speak-brief and other HTML comments in place. Returns the new length.
size_t se_strip_comments(char *text, size_t len);

// Reduce Markdown inline syntax in place: images disappear, links keep their
// label, emphasis and code markers are removed. Returns the new length.
size_t se_strip_inline(char *text, size_t len);

// Whispers hallucinate stock phrases in silence. If the tail of `text` matches
// one of `phrases`, cut from that match to the end. A phrase in the middle of a
// real sentence is left alone: never drop the whole transcript over one hit.
//
// `phrases` is an array of NUL-terminated strings. Returns the new length.
size_t se_trim_hallucination(char *text, size_t len,
                             const char *const *phrases, int phrase_count);

#ifdef __cplusplus
}
#endif
