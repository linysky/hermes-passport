// main/adpcm.c — IMA-ADPCM codec. See adpcm.h for the rationale.

#include "adpcm.h"

// ── Standard IMA-ADPCM tables ───────────────────────────────────────────────

static const int kIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

static const int kStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

#define STEP_INDEX_MAX 88

// ── State ───────────────────────────────────────────────────────────────────

void adpcm_reset(adpcm_state_t *s)
{
    if (!s) return;
    s->predictor = 0;
    s->index = 0;
}

void adpcm_on_overflow(adpcm_state_t *s)
{
    // Same as a reset, but named so the call site documents WHY: the stream has
    // a gap and continuing against the stale predictor would decode garbage.
    adpcm_reset(s);
}

size_t adpcm_encoded_bytes(size_t samples)
{
    return (samples + 1) / 2;
}

// ── One code ────────────────────────────────────────────────────────────────

// Advance the state as a decoder would for `nibble`. Encoder and decoder MUST
// do exactly the same thing or they drift apart within one block.
static void decode_nibble(adpcm_state_t *s, uint8_t nibble)
{
    int step = kStepTable[s->index];

    int diff = step >> 3;
    if (nibble & 4) diff += step;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 1) diff += step >> 2;

    // Accumulate in `int`, not int16_t: `predictor + diff` can exceed the
    // sample range, and signed overflow is undefined behaviour. Clamping only
    // makes sense before narrowing back to int16_t.
    int pred = s->predictor;
    if (nibble & 8) pred -= diff;
    else            pred += diff;

    if (pred > 32767)  pred = 32767;
    if (pred < -32768) pred = -32768;
    s->predictor = (int16_t)pred;

    int idx = s->index + kIndexTable[nibble & 0x0F];
    if (idx < 0) idx = 0;
    if (idx > STEP_INDEX_MAX) idx = STEP_INDEX_MAX;
    s->index = (int8_t)idx;
}

static uint8_t encode_sample(adpcm_state_t *s, int16_t sample)
{
    int step = kStepTable[s->index];
    int diff = (int)sample - (int)s->predictor;

    uint8_t nibble = 0;
    if (diff < 0) {
        nibble = 8;
        diff = -diff;
    }
    if (diff >= step)        { nibble |= 4; diff -= step; }
    if (diff >= (step >> 1)) { nibble |= 2; diff -= step >> 1; }
    if (diff >= (step >> 2)) { nibble |= 1; }

    // Advance exactly as the decoder would, so both sides stay in step.
    decode_nibble(s, nibble);
    return nibble;
}

// ── Blocks ──────────────────────────────────────────────────────────────────

size_t adpcm_encode(adpcm_state_t *s, const int16_t *pcm, size_t samples,
                    uint8_t *out, size_t out_cap)
{
    if (!s || !pcm || !out) return 0;

    size_t produced = 0;
    for (size_t i = 0; i < samples; i += 2) {
        if (produced >= out_cap) break;

        uint8_t lo = encode_sample(s, pcm[i]);
        uint8_t hi = 0;
        if (i + 1 < samples) hi = encode_sample(s, pcm[i + 1]);

        // First sample in the low nibble.
        out[produced++] = (uint8_t)((hi << 4) | lo);
    }
    return produced;
}

size_t adpcm_decode(adpcm_state_t *s, const uint8_t *in, size_t bytes,
                    int16_t *pcm, size_t max_samples)
{
    if (!s || !in || !pcm) return 0;

    size_t produced = 0;
    for (size_t i = 0; i < bytes; i++) {
        uint8_t byte = in[i];

        if (produced < max_samples) {
            decode_nibble(s, (uint8_t)(byte & 0x0F));   // low nibble first
            pcm[produced++] = s->predictor;
        }
        if (produced < max_samples) {
            decode_nibble(s, (uint8_t)(byte >> 4));
            pcm[produced++] = s->predictor;
        }
    }
    return produced;
}
