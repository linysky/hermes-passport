// main/history_ring.c — 10-slot ring buffer of reply summaries. See history_ring.h.

#include "history_ring.h"

#include <string.h>

#include "text_layout.h"   // for tl_utf8_decode: truncate on character boundaries

void hr_init(hr_ring_t *r)
{
    if (!r) return;
    memset(r, 0, sizeof *r);
    r->newest = -1;
}

size_t hr_truncate_utf8(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return 0;
    dst[0] = '\0';
    if (!src) return 0;

    size_t len = strlen(src);
    size_t out = 0;
    size_t p = 0;

    while (p < len) {
        uint32_t cp = 0;
        size_t adv = tl_utf8_decode(src + p, len - p, &cp);
        if (adv == 0) { adv = 1; cp = 0xFFFDu; }   // resynchronise on bad bytes

        // Leave room for the NUL. Refuse the character if it will not fit whole.
        if (out + adv + 1 > cap) break;

        memcpy(dst + out, src + p, adv);
        out += adv;
        p += adv;
    }

    dst[out] = '\0';
    return out;
}

int hr_find_id(const hr_ring_t *r, uint32_t id)
{
    if (!r || r->count == 0) return -1;
    for (int i = 0; i < r->count; i++) {
        int idx = (r->newest - i + HR_SLOTS) % HR_SLOTS;
        if (r->slots[idx].id == id) return i;
    }
    return -1;
}

bool hr_push(hr_ring_t *r, uint32_t id, const char *text, uint32_t ts)
{
    if (!r || !text) return false;

    // Dedup: a reconnect replay must not occupy a second slot.
    if (hr_find_id(r, id) >= 0) return false;

    int idx;
    if (r->count < HR_SLOTS) {
        idx = (r->newest + 1) % HR_SLOTS;
        r->count++;
    } else {
        // Overwrite the oldest slot.
        idx = (r->newest + 1) % HR_SLOTS;
    }

    hr_slot_t *s = &r->slots[idx];
    memset(s, 0, sizeof *s);
    hr_truncate_utf8(s->text, sizeof s->text, text);
    s->ts = ts;
    s->id = id;

    r->newest = idx;
    r->last_id = id;
    return true;
}

bool hr_get(const hr_ring_t *r, int index, hr_slot_t *out)
{
    if (!r || !out || index < 0 || index >= r->count) return false;
    int idx = (r->newest - index + HR_SLOTS * 2) % HR_SLOTS;
    *out = r->slots[idx];
    return true;
}

int hr_count(const hr_ring_t *r)
{
    return r ? r->count : 0;
}
