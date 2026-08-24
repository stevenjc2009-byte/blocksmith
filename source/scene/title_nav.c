#include "scene/title_nav.h"

// Leaving and entering are decided together because they are not independent, which is exactly
// what the two separate `if`s in scene/title.c missed. BACK set the screen to TITLE_SCR_MAIN and
// then fell through into the entry gate twenty lines below, so a frame that was both a BACK and
// an open gate produced both — and main.c acts on the action, so the player got the server's
// world instead of the screen they asked for.
//
// The window is not theoretical and it is not one frame wide. "Joined - syncing block table..."
// invites the player to sit through a wait of up to NETWORLD_REG_SYNC_DEADLINE_MS (2000 ms), and
// networldRegistryWaiting() flips to false on whichever frame the last BS_APP_REGISTRY_DEFS
// batch lands or the deadline expires. B pressed on that frame is an ordinary thing for a player
// who has decided the wait is too long to do.
//
// Leaving wins rather than entering, because the two are not the same kind of fact: BACK is
// something the player asked for, and the gate is a condition that merely stopped being closed.
//
// DISCONNECT needed nothing here and still does not: netDisconnect() reaches networldInit(),
// which clears the world seed, so have_world_seed is already false on the frame that button was
// pressed. This is the same shape one step earlier — the intent disarms the gate — rather than
// a second mechanism.
TitleMpNavOut titleMpNav(TitleMpNav in)
{
	TitleMpNavOut out = {false, false};

	if (in.back) {
		out.leave_to_main = true;
		return out;
	}

	if (in.connected && in.have_world_seed && !in.registry_waiting)
		out.start_server = true;

	return out;
}
