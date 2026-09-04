#pragma once

// ── diagRetire(): move one boot's diagnostic file out of the way of the next ──────────────
//
// Moved out of main.c on 2026-09-04 so tests/diag_retire_test.c can link the REAL function
// instead of a hand-copied twin — main.c pulls in <3ds.h> at its very first #include and can
// never be compiled as a whole on the host. remove()/rename()/fopen() are plain C stdio, not
// 3DS-specific, so this file needs no __3DS__ guard at all; it is identical on both targets.
//
// This exists because of a specific and expensive mistake. steve booted v1.2.0 twice — once
// into multiplayer, once into single player — and both froze. The card came back with
// postmortem.txt saying "frame 2, GPU wedged" and hang.txt saying "340 frames drawn, phase
// DRAW". Both stamped 1.2.0. Both cannot describe the same freeze, and nothing on the card said
// which boot either came from, so two separate failures were read as one and the fix that
// followed was aimed at an average of them.
//
// After the fence runs, any UN-prefixed diagnostic file on the card is from the boot that
// just froze, full stop. A boot that freezes writes files and cannot clear them; the next
// boot's fence moves them out of the way before it can write its own. There is no third
// possibility and no way to be looking at last time's evidence under this boot's name.
//
// drawprobe.txt is deliberately never passed through diagRetire(): it is the one file whose
// whole job is to survive a boot, because a boot that hangs never writes its own survival
// line and the silence is what the next boot reads. bootid.txt is written last by the caller,
// so a card with diagnostic files but no bootid.txt means the fence itself failed and nothing
// on it should be trusted.
//
// ── 2026-09-03: this fence used to DELETE. It now renames to prev-*. ───────────────────────
//
// The invariant above is unchanged and is the whole reason this function exists: an
// un-prefixed diagnostic file still means "written by the boot that just froze, full stop",
// because the fence still moves every one of them out of the way before anything can write.
//
// What changed is what happens to the PREVIOUS boot's copy, and it changed because the old
// answer — destroy it — cost us the only evidence of the only freeze we currently care about.
// steve froze a real console at render distance 5 after walking one or two chunks from spawn.
// hang.txt on his card is the single most informative artefact in this entire investigation, it
// exists in exactly one copy, and until this edit the act of launching the game to reproduce the
// bug was also the act of erasing the record of it.
//
// One generation, not a rotation. prev-hang.txt is meant to be overwritten only when the next
// boot has a new one to put there, so this buys exactly one accidental relaunch, which is the
// failure mode that actually happens — the console froze, you power-cycled it, you launched
// the game again before it occurred to you that the card had anything on it. Keeping more
// would mean managing a ring on a user's SD card for a benefit nobody has ever needed.
//
// The remove-then-rename order, when there IS something to retire, is not defensive noise:
// FAT rename() refuses an existing target, so without the remove a second freeze in a row
// would silently keep the FIRST one's prev- file and quietly discard the newer one — the exact
// ambiguity this fence exists to prevent, reintroduced through the back door. The fallback to
// remove() on a failed rename preserves the old invariant rather than leaving a stale file
// lying around claiming to be this boot's.
//
// ── 2026-09-04: retirement is now CONDITIONAL, because it still wasn't one generation ──────
//
// The paragraph above says prev-<name> "is meant to be overwritten only when the next boot
// has a new one to put there" — the 2026-09-03 fix never actually made that true. It called
// remove(prev) unconditionally, every boot, before even checking whether `live` existed. A
// boot that did NOT freeze has no hang.txt to rename, so the rename failed (ENOENT), the
// remove(live) fallback was a no-op, and the net effect of that boot's fence was: destroy
// prev-hang.txt, replace it with nothing. Two boots after a real freeze — one clean relaunch
// to retire it into prev-, one MORE clean relaunch or reinstall to wipe it out again — and the
// evidence was gone with no freeze involved in the second step at all.
//
// This is exactly what happened to steve's v1.8.16 freeze: v1.8.17 shipped, he relaunched more
// than once while diagnosing, and prev-hang.txt / prev-postmortem.txt were both gone by the
// time anyone went looking, leaving only prev-bootid.txt. The v1.8.16 report survives today
// purely because a human read it off the card and pasted it into a commit message before the
// evidence was overwritten.
//
// The fix: diagRetire() now probes for `live` first (a plain fopen(), which is what "does this
// boot have anything to retire" actually means) and does nothing at all — remove included — if
// there is nothing there. A clean boot is now a true no-op for this file: prev-<name> is left
// exactly as the last boot that actually retired something left it, however many clean boots
// pass in between. This makes depth 1 (one generation, unchanged from 2026-09-03) sufficient
// for the pattern actually observed — two freezes in a row, not two freezes separated by an
// arbitrary number of clean relaunches — because the freeze-then-freeze case now falls out for
// free: boot N freezes and writes hang.txt; boot N+1's fence retires it to prev-hang.txt before
// boot N+1 does anything else; if boot N+1 ALSO freezes, it writes a fresh hang.txt alongside
// the already-retired prev-hang.txt, and both survive side by side. A ring deeper than 1 would
// only matter for three or more consecutive freezes with no clean boot anywhere in between,
// which is not the pattern on record and would cost SD file count for a benefit nobody has
// asked for — same reasoning as 2026-09-03, now actually delivered.
//
// diagShouldRetire() is split out as a pure yes/no so the POLICY ("is there something worth
// retiring") is testable on the host with no filesystem at all, separately from the shell
// (fopen/remove/rename) around it. See tests/diag_retire_test.c for the boot-1-freeze,
// boot-2-clean, boot-3-clean scenario this was actually written to fix.

#include <stdbool.h>

// Pure policy: given whether `live` exists, should this boot's fence retire it to `prev`?
// Currently just "yes iff there is something there" — kept as a named seam rather than an
// inline check because it is the one line a future depth->1 change would touch, and because
// it is what tests/diag_retire_test.c exercises directly with no filesystem involved.
bool diagShouldRetire(bool live_exists);

// Retires one diagnostic file: if `live` exists, moves it to `prev` (removing whatever was at
// `prev` first, since FAT rename() refuses an existing target). If `live` does not exist,
// leaves `prev` untouched — see the 2026-09-04 note above for why that used to not be true.
//
// remove()/rename()/fopen() are portable C stdio; nothing here is 3DS-specific, so this
// function runs identically whether `live`/`prev` are "sdmc:/blocksmith/..." paths on a
// console or ordinary temp-directory paths in tests/diag_retire_test.c.
void diagRetire(const char* live, const char* prev);
