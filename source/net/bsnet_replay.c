#include "bsnet_replay.h"

#include <string.h>

static void set_bit(struct bsnet_replay *r, uint64_t distance)
{
    uint64_t idx = distance % BS_REPLAY_WINDOW;
    r->bits[idx / 64u] |= (uint64_t)1 << (idx % 64u);
}

static bool get_bit(const struct bsnet_replay *r, uint64_t distance)
{
    uint64_t idx = distance % BS_REPLAY_WINDOW;
    return (r->bits[idx / 64u] >> (idx % 64u)) & 1u;
}

void bsnet_replay_init(struct bsnet_replay *r)
{
    memset(r, 0, sizeof *r);
}

bool bsnet_replay_check(struct bsnet_replay *r, uint64_t msg_id)
{
    if (!r->any) {
        r->any     = true;
        r->highest = msg_id;
        memset(r->bits, 0, sizeof r->bits);
        set_bit(r, msg_id);
        return true;
    }

    if (msg_id > r->highest) {
        uint64_t advance = msg_id - r->highest;

        if (advance >= BS_REPLAY_WINDOW) {
            memset(r->bits, 0, sizeof r->bits);
        } else {
            for (uint64_t d = 1; d <= advance; d++) {
                uint64_t idx = (r->highest + d) % BS_REPLAY_WINDOW;
                r->bits[idx / 64u] &= ~((uint64_t)1 << (idx % 64u));
            }
        }

        r->highest = msg_id;
        set_bit(r, msg_id);
        return true;
    }

    if (r->highest - msg_id >= BS_REPLAY_WINDOW) {
        return false;
    }
    if (get_bit(r, msg_id)) {
        return false;
    }

    set_bit(r, msg_id);
    return true;
}
