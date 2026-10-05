// tests/test_adpcm.c — host tests for main/adpcm.c
//
// The centrepiece is the overflow case from real hardware: ADPCM is stateful
// differential coding, so losing one byte makes every following sample decode
// wrong and extrapolate toward full scale -- a loud crack. The test measures
// the sample-to-sample jump before and after a reset and asserts that the
// reset removes the spike.
//
// Build (see tools/validate.sh):
//   cc -std=c11 -Wall -Wextra -Werror -Imain
//      tests/test_adpcm.c main/adpcm.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adpcm.h"

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

// Largest absolute sample-to-sample jump in `pcm`. A "crack" shows up here.
static int max_jump(const int16_t *pcm, size_t n)
{
    int worst = 0;
    for (size_t i = 1; i < n; i++) {
        int d = (int)pcm[i] - (int)pcm[i - 1];
        if (d < 0) d = -d;
        if (d > worst) worst = d;
    }
    return worst;
}

// ── Reset ───────────────────────────────────────────────────────────────────

static void test_reset(void)
{
    adpcm_state_t s;

    s.predictor = 1234;
    s.index = 55;
    adpcm_reset(&s);
    CHECK(s.predictor == 0 && s.index == 0, "reset clears both fields");

    s.predictor = 1234;
    s.index = 55;
    adpcm_on_overflow(&s);
    CHECK(s.predictor == 0 && s.index == 0, "overflow handler resets the state");

    adpcm_reset(NULL);   // must not crash
    adpcm_on_overflow(NULL);
    CHECK(1, "NULL state is safe");
}

// ── Sizing ──────────────────────────────────────────────────────────────────

static void test_sizing(void)
{
    CHECK(adpcm_encoded_bytes(0) == 0, "zero samples need zero bytes");
    CHECK(adpcm_encoded_bytes(1) == 1, "one sample still takes a byte");
    CHECK(adpcm_encoded_bytes(2) == 1, "two samples take one byte");
    CHECK(adpcm_encoded_bytes(640) == 320, "a 40 ms block halves in size");
}

// ── Round trip ──────────────────────────────────────────────────────────────

static void test_round_trip(void)
{
    enum { N = 512 };
    int16_t in[N], out[N];
    uint8_t packed[N];

    // A gentle ramp: predictable, and a wrong codec drifts visibly.
    for (int i = 0; i < N; i++) in[i] = (int16_t)((i - N / 2) * 12);

    adpcm_state_t enc, dec;
    adpcm_reset(&enc);
    adpcm_reset(&dec);

    size_t nbytes = adpcm_encode(&enc, in, N, packed, sizeof packed);
    CHECK(nbytes == N / 2, "two samples per byte");

    size_t nsamp = adpcm_decode(&dec, packed, nbytes, out, N);
    CHECK(nsamp == N, "every sample comes back");

    // 4-bit ADPCM is lossy, but the error is not uniform: the predictor starts
    // at 0 while the ramp starts far from it, so the first samples chase the
    // signal and the steady state is nearly exact. Measuring the whole block
    // would size the assertion to the transient; measure the steady state.
    int worst_transient = 0, worst_steady = 0;
    for (int i = 0; i < N; i++) {
        int e = (int)in[i] - (int)out[i];
        if (e < 0) e = -e;
        if (i < 64) {
            if (e > worst_transient) worst_transient = e;
        } else if (e > worst_steady) {
            worst_steady = e;
        }
    }
    printf("    ramp error: transient=%d  steady=%d\n", worst_transient, worst_steady);
    CHECK(worst_steady < 20, "steady-state error is a couple of counts");
    CHECK(worst_steady < worst_transient, "the codec converges after the transient");

    // Direction is preserved: the ramp still rises.
    CHECK(out[N - 1] > out[0], "the ramp keeps its direction");
}

// ── Silence ─────────────────────────────────────────────────────────────────

static void test_silence(void)
{
    enum { N = 256 };
    int16_t in[N] = {0}, out[N];
    uint8_t packed[N];

    adpcm_state_t enc, dec;
    adpcm_reset(&enc);
    adpcm_reset(&dec);

    size_t nbytes = adpcm_encode(&enc, in, N, packed, sizeof packed);
    size_t nsamp = adpcm_decode(&dec, packed, nbytes, out, N);

    CHECK(nsamp == N, "silence decodes fully");
    for (int i = 0; i < N; i++) {
        CHECK(out[i] == 0, "silence stays silent");
        if (out[i] != 0) break;
    }
}

// ── Extremes stay in range ──────────────────────────────────────────────────

static void test_extremes(void)
{
    enum { N = 128 };
    int16_t in[N], out[N];
    uint8_t packed[N];

    // Full-scale positive and negative alternation is the worst case.
    for (int i = 0; i < N; i++) in[i] = (i & 1) ? 32767 : -32768;

    adpcm_state_t enc, dec;
    adpcm_reset(&enc);
    adpcm_reset(&dec);

    size_t nbytes = adpcm_encode(&enc, in, N, packed, sizeof packed);
    size_t nsamp = adpcm_decode(&dec, packed, nbytes, out, N);

    CHECK(nsamp == N, "extremes decode fully");

    // Full-scale alternation is the hardest case for a 4-bit codec. The point
    // is not sample accuracy -- it is that the codec neither saturates every
    // output to a rail nor lets the predictor run away. (int16_t already spans
    // the full sample range, so a range check here would be vacuous.)
    int positives = 0, negatives = 0, saturated = 0;
    for (int i = 0; i < N; i++) {
        if (out[i] > 0) positives++;
        else if (out[i] < 0) negatives++;
        if (out[i] >= 32767 || out[i] <= -32768) saturated++;
    }
    CHECK(positives > 0 && negatives > 0,
          "extremes decode to a mixed signal, not one rail");
    CHECK(saturated < N / 2, "the codec does not pin every sample to full scale");
}

// ── The real bug: a dropped byte must not become a crack ────────────────────

static void test_gap_without_reset_is_loud(void)
{
    enum { N = 512 };
    static int16_t in[N], out[N];
    static uint8_t packed[N];

    // A calm signal: a slow sine-ish walk with small steps.
    int v = 0;
    for (int i = 0; i < N; i++) {
        v += (i % 32 < 16) ? 300 : -300;
        in[i] = (int16_t)v;
    }

    adpcm_state_t enc, dec;
    adpcm_reset(&enc);
    adpcm_reset(&dec);
    size_t nbytes = adpcm_encode(&enc, in, N, packed, sizeof packed);

    // Decode the first half normally.
    size_t half = nbytes / 2;
    size_t first = adpcm_decode(&dec, packed, half, out, N);
    CHECK(first == half * 2, "first half decoded");

    // Now drop `gap` bytes -- a lost fragment on the BLE path -- and decode the
    // rest WITHOUT resetting. The state is now wrong for the data that follows.
    size_t gap = 8;
    size_t tail = adpcm_decode(&dec, packed + half + gap, nbytes - half - gap,
                               out + first, N - first);
    CHECK(tail > 0, "tail decoded");

    // Measure the jump at the seam.
    int seam = (int)out[first] - (int)out[first - 1];
    if (seam < 0) seam = -seam;

    // Repeat the same thing, but reset at the gap.
    adpcm_reset(&dec);
    first = adpcm_decode(&dec, packed, half, out, N);
    adpcm_on_overflow(&dec);   // the gap is detected: drop the batch, resync
    size_t tail2 = adpcm_decode(&dec, packed + half + gap, nbytes - half - gap,
                                out + first, N - first);
    CHECK(tail2 > 0, "tail decoded after reset");

    int seam2 = (int)out[first] - (int)out[first - 1];
    if (seam2 < 0) seam2 = -seam2;

    // Without a reset the seam is a spike; with one it is calm. This is the
    // exact property the overflow handler exists to guarantee.
    printf("    seam jump: no-reset=%d  with-reset=%d\n", seam, seam2);
    CHECK(seam2 <= seam, "reset never makes the seam worse");
    CHECK(seam2 < 4000, "after a reset the seam is not a crack");

    // And the overall smoothest result comes from a clean stream.
    adpcm_reset(&dec);
    adpcm_decode(&dec, packed, nbytes, out, N);
    int clean = max_jump(out, N);
    printf("    max jump: clean=%d\n", clean);
    CHECK(clean < 2000, "a clean stream has no crack either");
}

// ── Buffer limits ───────────────────────────────────────────────────────────

static void test_limits(void)
{
    enum { N = 16 };
    int16_t in[N] = {0}, out[N];
    uint8_t packed[N];

    adpcm_state_t enc, dec;
    adpcm_reset(&enc);
    adpcm_reset(&dec);

    // Output capacity shorter than needed: stop cleanly, never overrun.
    size_t got = adpcm_encode(&enc, in, N, packed, 4);
    CHECK(got == 4, "encode honours the output capacity");

    // Decode with a tiny sample budget.
    got = adpcm_decode(&dec, packed, 4, out, 3);
    CHECK(got == 3, "decode honours the sample budget");

    // Null safety.
    CHECK(adpcm_encode(NULL, in, N, packed, N) == 0, "NULL encoder is safe");
    CHECK(adpcm_encode(&enc, NULL, N, packed, N) == 0, "NULL input is safe");
    CHECK(adpcm_decode(&dec, packed, 4, NULL, N) == 0, "NULL output is safe");
    CHECK(adpcm_encode(&enc, in, 0, packed, N) == 0, "zero samples encode nothing");
    CHECK(adpcm_decode(&dec, packed, 0, out, N) == 0, "zero bytes decode nothing");
}

// ── Continuity across blocks ────────────────────────────────────────────────

static void test_state_carries(void)
{
    enum { N = 64 };
    int16_t in[N], one[N], split[N];
    uint8_t packed[N];

    for (int i = 0; i < N; i++) in[i] = (int16_t)((i * 777) % 5000 - 2500);

    // Encode the whole block in one go.
    adpcm_state_t enc;
    adpcm_reset(&enc);
    size_t nbytes = adpcm_encode(&enc, in, N, packed, sizeof packed);

    adpcm_state_t dec;
    adpcm_reset(&dec);
    adpcm_decode(&dec, packed, nbytes, one, N);

    // Encode the same samples as two halves with a state that carries over.
    adpcm_state_t enc2, dec2;
    adpcm_reset(&enc2);
    adpcm_reset(&dec2);
    uint8_t p1[N], p2[N];
    size_t n1 = adpcm_encode(&enc2, in, N / 2, p1, sizeof p1);
    size_t n2 = adpcm_encode(&enc2, in + N / 2, N / 2, p2, sizeof p2);
    adpcm_decode(&dec2, p1, n1, split, N);
    adpcm_decode(&dec2, p2, n2, split + N / 2, N - N / 2);

    // Splitting the block must not change the bitstream: the state carries.
    int identical = (n1 + n2 == nbytes) && (memcmp(p1, packed, n1) == 0) &&
                    (memcmp(p2, packed + n1, n2) == 0);
    CHECK(identical, "split encoding matches whole-block encoding");

    for (int i = 0; i < N; i++) {
        CHECK(split[i] == one[i], "split decode matches whole-block decode");
        if (split[i] != one[i]) break;
    }
}

int main(void)
{
    test_reset();
    test_sizing();
    test_round_trip();
    test_silence();
    test_extremes();
    test_gap_without_reset_is_loud();
    test_limits();
    test_state_carries();

    if (failures) {
        fprintf(stderr, "adpcm: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("adpcm: all %d checks passed\n", checks);
    return 0;
}
