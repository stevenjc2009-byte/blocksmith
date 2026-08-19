/* bsnet_replay.h — client-side mirror of the gateway's sliding replay window.
 *
 * server/gateway/replay.{c,h} implements the server's half of this and is not
 * shared code (bs_proto.h is the only header documented as dependency-free and
 * shared verbatim; replay.c is gateway-only, same as allowlist.c). The client
 * needs the identical algorithm applied to the S2C stream, so it is
 * reproduced here rather than linked from server/gateway/, which keeps this
 * build independent of the gateway's object files and Makefile.
 *
 * Semantics are exactly server/gateway/replay.c's: `highest` is the largest
 * message id accepted so far, `bits` records which of the preceding
 * BS_REPLAY_WINDOW ids were seen. Call only after the AEAD tag has verified —
 * a forged id must never be allowed to poison the window.
 */

#ifndef BSNET_REPLAY_H
#define BSNET_REPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "proto/bs_proto.h"

#define BSNET_REPLAY_WORDS (BS_REPLAY_WINDOW / 64u)

struct bsnet_replay {
    uint64_t highest;
    uint64_t bits[BSNET_REPLAY_WORDS];
    bool     any;
};

void bsnet_replay_init(struct bsnet_replay *r);
bool bsnet_replay_check(struct bsnet_replay *r, uint64_t msg_id);

#endif /* BSNET_REPLAY_H */
