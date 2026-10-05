// main/adpcm.h — IMA-ADPCM codec for the BLE audio path.
//
// Pure C11, no ESP-IDF, no LVGL, no heap. Covered by tests/test_adpcm.c.
//
// Why ADPCM and not raw PCM16:
//   IMA-ADPCM packs two samples into one byte (4 bits each), so the BLE path
//   carries 4x less traffic than raw PCM16 at the same 16 kHz. That matters on
//   a link with unacknowledged writes.
//
// Why the reset function exists (this is a real bug, not a hypothetical):
//   ADPCM is STATEFUL differential coding. If a byte is lost on the way, every
//   following sample decodes wrong and extrapolates toward full scale -- the
//   wearer hears a loud crack. On a detected gap the caller must reset the
//   decoder state and drop that batch: a few milliseconds of silence beats a
//   burst of noise.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Codec state. One per stream, in each direction.
typedef struct {
    int16_t predictor;   // last reconstructed sample
    int8_t index;        // current step-size index, 0..88
} adpcm_state_t;

// Start of a clean stream, and also what to call after a detected gap.
void adpcm_reset(adpcm_state_t *s);

// Encode `samples` PCM16 samples into `out`. Each output byte holds two 4-bit
// codes, the first sample in the low nibble. Returns bytes written; the caller
// must provide samples/2 (rounded up) bytes.
//
// `samples` should be even for a clean byte boundary. An odd trailing sample is
// still encoded and occupies a whole byte (its high nibble is zero).
size_t adpcm_encode(adpcm_state_t *s, const int16_t *pcm, size_t samples,
                    uint8_t *out, size_t out_cap);

// Decode `bytes` ADPCM bytes into up to `max_samples` PCM16 samples. Returns
// samples written. Every byte yields two samples.
size_t adpcm_decode(adpcm_state_t *s, const uint8_t *in, size_t bytes,
                    int16_t *pcm, size_t max_samples);

// Call when the stream is known to have a gap (dropped bytes, resync, buffer
// overflow). Resets the state so the next sample does not decode against a
// stale predictor. The caller should also discard the batch that contained the
// gap rather than play it.
void adpcm_on_overflow(adpcm_state_t *s);

// Bytes needed to encode `samples` samples (two samples per byte).
size_t adpcm_encoded_bytes(size_t samples);

#ifdef __cplusplus
}
#endif
