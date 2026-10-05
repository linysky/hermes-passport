// tests/test_summary_extract.c — host tests for main/summary_extract.c
//
// The cases below are the real behaviours this module exists for:
//   * the speak-brief block must never reach the screen;
//   * a summary must land on a UTF-8 character boundary;
//   * the hallucination guard trims only the tail, never the whole sentence.
//
// Build (see tools/validate.sh):
//   cc -std=c11 -Wall -Wextra -Werror -Imain
//      tests/test_summary_extract.c main/summary_extract.c main/text_layout.c

#include <stdio.h>
#include <string.h>

#include "summary_extract.h"
#include "text_layout.h"

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

#define CHECK_STR(got, want, msg)                                            \
    do {                                                                     \
        checks++;                                                            \
        if (strcmp((got), (want)) != 0) {                                    \
            fprintf(stderr, "FAIL %s:%d  %s\n    got  =\"%s\"\n    want=\"%s\"\n", \
                    __FILE__, __LINE__, (msg), (got), (want));               \
            failures++;                                                      \
        }                                                                    \
    } while (0)

// ── Comments ────────────────────────────────────────────────────────────────

static void test_comments(void)
{
    char buf[256];

    // The speak-brief block is for the voice layer; the screen must not show it.
    strcpy(buf, "<!--SPK 明天天气 洛杉矶 -->\n明天多云转晴。");
    se_strip_comments(buf, strlen(buf));
    CHECK_STR(buf, "\n明天多云转晴。", "SPK comment removed");

    // A malformed comment that never closes still disappears, and only to the
    // end of its line -- the rest of the reply survives.
    strcpy(buf, "<!--SPK 明天天气\n下一行内容。");
    se_strip_comments(buf, strlen(buf));
    CHECK_STR(buf, "\n下一行内容。", "unterminated comment dropped to end of line");

    // Ordinary text is untouched.
    strcpy(buf, "普通文本，没有注释。");
    se_strip_comments(buf, strlen(buf));
    CHECK_STR(buf, "普通文本，没有注释。", "plain text unchanged");

    // An empty comment.
    strcpy(buf, "a<!---->b");
    se_strip_comments(buf, strlen(buf));
    CHECK_STR(buf, "ab", "empty comment removed");
}

// ── Inline Markdown ─────────────────────────────────────────────────────────

static void test_inline(void)
{
    char buf[256];

    strcpy(buf, "见 [天气](http://x/y) 就好");
    se_strip_inline(buf, strlen(buf));
    CHECK_STR(buf, "见 天气 就好", "link keeps its label");

    strcpy(buf, "看 ![图](http://x/y.png) 图");
    se_strip_inline(buf, strlen(buf));
    CHECK_STR(buf, "看  图", "image removed entirely");

    strcpy(buf, "用 `code` 试试");
    se_strip_inline(buf, strlen(buf));
    CHECK_STR(buf, "用 code 试试", "inline code keeps its content");

    strcpy(buf, "```json\n{\"a\": 1}\n```");
    se_strip_inline(buf, strlen(buf));
    CHECK_STR(buf, "", "fenced code block removed");

    strcpy(buf, "前 ```\ncode\n``` 后");
    se_strip_inline(buf, strlen(buf));
    CHECK_STR(buf, "前  后", "fenced block removed between words");
}

// ── Summary extraction ──────────────────────────────────────────────────────

static void test_summary(void)
{
    char out[SE_SUMMARY_MAX_BYTES];

    // First paragraph only.
    const char *reply = "第一段内容，介绍结果。\n\n第二段是细节，不该上屏。";
    size_t n = se_summary(reply, strlen(reply), out, sizeof out);
    CHECK(n > 0, "summary produced");
    CHECK_STR(out, "第一段内容，介绍结果。", "only the first paragraph is kept");

    // A TITLE: line wins over the preamble.
    const char *titled = "这是一段很长的开场白，说的是别的事。\n\nTITLE: 明天里斯本天气\n正文细节。";
    se_summary(titled, strlen(titled), out, sizeof out);
    CHECK_STR(out, "明天里斯本天气", "TITLE line is preferred");

    // Leading blank lines are skipped.
    const char *padded = "\n\n  正文从这里开始。";
    se_summary(padded, strlen(padded), out, sizeof out);
    CHECK_STR(out, "正文从这里开始。", "leading whitespace skipped");

    // Comments and code are gone before extraction.
    const char *messy = "<!--SPK 播报要点 -->\n```\ncode\n```\n真正的摘要。";
    se_summary(messy, strlen(messy), out, sizeof out);
    CHECK_STR(out, "真正的摘要。", "comments and code stripped before extraction");
}

// ── Truncation stays on character boundaries ────────────────────────────────

static void test_truncation(void)
{
    char out[8];

    // 3 CJK characters need 9 bytes + NUL. Capacity 8 holds two.
    const char *text = "中文字";
    size_t n = se_summary(text, strlen(text), out, sizeof out);
    CHECK(n == 6, "truncated to two whole characters");
    CHECK_STR(out, "中文", "cut lands between characters");

    // Whatever survives must be re-decodable.
    size_t p = 0;
    while (p < n) {
        uint32_t cp = 0;
        size_t adv = tl_utf8_decode(out + p, n - p, &cp);
        CHECK(adv > 0, "output stays on character boundaries");
        if (adv == 0) break;
        p += adv;
    }

    // Degenerate capacities.
    char tiny[1];
    CHECK(se_summary(text, strlen(text), tiny, 1) == 0, "capacity 1 yields nothing");
    CHECK(tiny[0] == '\0', "capacity 1 still NUL-terminates");
    CHECK(se_summary(text, strlen(text), NULL, 8) == 0, "NULL output is safe");
    CHECK(se_summary(NULL, 0, tiny, 1) == 0, "NULL input is safe");
}

// ── Hallucination guard: trim the tail, never the sentence ──────────────────

static void test_hallucination(void)
{
    const char *phrases[] = {
        "\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97\xe5\xb9\x95\xe5\xbf\x97\xe6\x84\xbf\xe8\x80\x85",  // the well-known one
        "thanks for watching",
    };

    char buf[256];

    // A tail match is removed.
    strcpy(buf, "明天多云转晴。thanks for watching");
    size_t n = se_trim_hallucination(buf, strlen(buf), phrases, 2);
    CHECK_STR(buf, "明天多云转晴。", "tail phrase trimmed");

    // A phrase in the middle of a real sentence is left alone. This is the
    // bug the guard exists for: dropping the whole transcript over one hit
    // would kill the real sentence too.
    strcpy(buf, "有人说 thanks for watching 是套话，但我不同意。");
    n = se_trim_hallucination(buf, strlen(buf), phrases, 2);
    CHECK_STR(buf, "有人说 thanks for watching 是套话，但我不同意。",
              "mid-sentence phrase is left alone");

    // Several tail phrases in a row.
    strcpy(buf, "结论。thanks for watching  ");
    n = se_trim_hallucination(buf, strlen(buf), phrases, 2);
    CHECK_STR(buf, "结论。", "repeated tail phrases trimmed, whitespace too");

    // Nothing to do.
    strcpy(buf, "干净的一句话。");
    n = se_trim_hallucination(buf, strlen(buf), phrases, 2);
    CHECK_STR(buf, "干净的一句话。", "clean text unchanged");
    CHECK(n == strlen("干净的一句话。"), "length reported correctly");

    // Null safety.
    CHECK(se_trim_hallucination(NULL, 0, phrases, 2) == 0, "NULL text is safe");
    CHECK(se_trim_hallucination(buf, strlen(buf), NULL, 2) == strlen(buf),
          "NULL phrase list is a no-op");
}

int main(void)
{
    test_comments();
    test_inline();
    test_summary();
    test_truncation();
    test_hallucination();

    if (failures) {
        fprintf(stderr, "summary_extract: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("summary_extract: all %d checks passed\n", checks);
    return 0;
}
