// One visit to one world, and the state that must not survive it — pulled out of
// source/main.c so it can be host-tested, the same split and the same reason as
// scene/title_nav.h (the multiplayer navigation rule out of scene/title.c) and
// app/whatsnew.h (note parsing out of title.c). main.c includes <3ds.h> and 40 other
// console headers and cannot be linked outside a devkitARM build; what was worth checking
// here was never a rectangle or a key constant, only a rule about lifetimes.
//
// The rule: main.c runs one session per lap of its `session_start:` label. A session ends
// in one of three ways — the pause menu's "Quit to title" in single player, a server
// session ending (Quit, a kick, a lost link, HOME), or the app shutting down — and the
// first two both come back to that label. Anything that is per-SESSION rather than
// per-BOOT has to be put back before the next lap can use it.
//
// The defect this file exists for (v1.6.0, F2). The block registry is per-session state:
// world/registry.c's table is frozen by genStart() before the worker starts, and only
// registryInitCore() unfreezes it. Leaving a SERVER session called netDisconnect(), which
// reaches networldInit(), which calls registryInitCore() — so that exit was covered by
// accident rather than by design. Leaving a SINGLE-PLAYER world called nothing at all:
// `if (quit_to_title) goto session_start;` jumped straight back to the label with the
// table still frozen and still holding that world's sidecar rows. The next thing the
// player could do was join a server, whose BS_APP_REGISTRY_DEFS then hit
// registryRemoteApply()'s `if (s_frozen) return 0;` and were refused, every batch, for
// the whole session. Every server-defined block became an invisible hole they walked
// through and fell into, and rebooting the console cleared it — which is what made it
// look intermittent. The same stale table also gave the NEXT single-player world the
// PREVIOUS one's blocks under the same ids.
//
// Why the reset lives at the START of the lap rather than at each exit: there are two
// `goto session_start;` statements and there was nothing stopping a third, so a fix that
// patched the exits would have to be remembered every time one is added. There is exactly
// one place that means "a new session is beginning", and it is also the only placement
// that is early enough — a joining client's DEFS arrive while the player is still on the
// multiplayer screen, inside the lap and before any world exists, so a reset done at
// world entry would already be too late.
#pragma once

// Called once per lap of main.c's session_start label, before the title screen. Puts
// per-session state that outlives a world back to its boot condition. Idempotent: on the
// first lap everything it touches is already fresh and it changes nothing.
//
// It does NOT call networldInit(). That would be the broader hammer, and main.c's own
// comment at netInit() records why networldInit() belongs at boot and not here: it clears
// the pending block-diff store, and a joined session's WORLD_SYNC batch arrives while the
// player is still looking at the menu. Leaving a session already clears it (netDisconnect
// in net/bsnet.c); the registry was the one piece nothing on the single-player path did.
void sessionBegin(void);
