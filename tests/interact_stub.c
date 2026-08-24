// Host-link stub for the one GPU symbol scene/interact.c calls.
//
// Why this file exists: interactEdit() ends every accepted edit with chunkRenderTouch()
// (scene/chunk_render.h), which is citro3d code and cannot compile on the PC. The call is
// unconditional in both builds and must stay that way — the ordering this module is tested
// for is exactly "write the block, relight, THEN tell the renderer", and an #ifdef inside
// interactEdit would let the console and the host run different orderings. So the absence
// lives in the link, not in the source. Same arrangement, and same reasoning, as
// tests/net_stub.c.
//
// This is a stub for a *renderer queue*, not for any part of the logic under test. What
// interact_test asserts — which cells the world holds afterwards, which id came back in
// broke_id, how many refusals were counted, whether a block edit went to the wire — is all
// produced by the real scene/interact.c. The only thing this file can influence is
// interactEdit's return value (chunks newly queued), and the test treats that as
// "0 in this build" rather than as evidence about the renderer.
//
// ⚠ Delete this file, and its line in tools/run_host_tests.sh, if scene/chunk_render.c ever
// becomes host-linkable — two definitions of the same symbol will not link, and the failure
// will name this file.

#include "world/world.h"

// The exact signature scene/chunk_render.h declares.
int chunkRenderTouch(const World* w, int x, int y, int z);

int chunkRenderTouch(const World* w, int x, int y, int z)
{
	(void)w;
	(void)x;
	(void)y;
	(void)z;
	return 0;
}
