// Host-link stubs for the multiplayer glue in source/net.
//
// Why this file exists: world.c's worldSet() calls networldOnColumnLoad() (net/networld.h)
// so a remote edit queued for a column that did not exist yet can land the moment it does.
// That is correct for the console build, where the Makefile puts source/net in SOURCES. The
// host suite does not link source/net — net/networld.c pulls in net/bsnet_transport.h, whose
// own header states it is not host-portable — so the world suite stopped linking with
//
//     source/world/world.c:201: undefined reference to `networldOnColumnLoad'
//
// and the save-format battery it guards stopped running entirely.
//
// A stub is the right shape here rather than an #ifdef in world.c, because world.c's logic is
// genuinely the same in both builds: the call is unconditional, and the *networking* is what
// is absent on the host, not the world write. Putting the absence in the link rather than in
// the source keeps world.c honest — it says what the program does, and the host build says
// what the host does not have.
//
// ⚠ Delete this file, and its line in tools/run_host_tests.sh, the moment source/net joins
// the host link — two definitions of the same symbol will not link, and the failure will name
// this file. It is deliberately in tests/ and deliberately listed next to world_test.c's own
// sources so that whoever adds net to that command sees it in the same screenful.
//
// Nothing in source/net is edited, read into, or depended on by this file. It reproduces one
// signature from net/networld.h and nothing else.

#include "world/world.h"

// A no-op with the exact signature net/networld.h declares. On the host there is no session,
// so there are no pending remote diffs to drain, so doing nothing is not a simplification of
// the real behaviour — it *is* the real behaviour for a client with no transport.
void networldOnColumnLoad(World* w, int cx, int cz);

void networldOnColumnLoad(World* w, int cx, int cz)
{
	(void)w;
	(void)cx;
	(void)cz;
}
