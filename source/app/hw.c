#include "app/hw.h"

// ── Pure policy ────────────────────────────────────────────────────────────────────────

int hwPreferredWorkerCore(bool new_3ds, bool cpu_time_limit_ok, int out[HW_CORE_LADDER_MAX])
{
	int n = 0;
	if (new_3ds)          out[n++] = 2;
	if (cpu_time_limit_ok) out[n++] = 1;
	out[n++] = 0;
	return n;
}

// ── The cached answer ──────────────────────────────────────────────────────────────────

static bool s_asked;
static bool s_new_3ds;
static bool s_speedup_requested;

bool hwIsNew3ds(void)         { return s_new_3ds; }
bool hwSpeedupRequested(void) { return s_speedup_requested; }

#ifdef __3DS__

#include <3ds.h>

void hwInit(void)
{
	if (s_asked) return;
	s_asked = true;

	bool n3ds = false;
	// A failed APT_CheckNew3DS leaves `n3ds` as we initialised it, which is the safe answer:
	// treating a New 3DS as an Old one costs performance, treating an Old one as New would
	// ask a console for a clock it does not have and pin a thread to a core that is not there.
	if (R_FAILED(APT_CheckNew3DS(&n3ds))) n3ds = false;
	s_new_3ds = n3ds;

	if (!s_new_3ds) return;

	// Both halves of the New 3DS speedup in one call: libctru's osSetSpeedupEnable(true)
	// asks PTM:SYSM for the 804 MHz clock AND the L2 cache together. It returns void, so
	// there is nothing to test and nothing to report beyond "we asked" — see
	// hwSpeedupRequested()'s comment in hw.h.
	//
	// This is a request on top of what cia/blocksmith.rsf's CpuSpeed and EnableL2Cache
	// already ask the loader for. The RSF covers the installed CIA; this covers the .3dsx
	// under the Homebrew Launcher, where the launcher's exheader is what the system read and
	// ours was never consulted. Doing both is not redundant — they are two different launch
	// paths and only one of them reads our exheader.
	osSetSpeedupEnable(true);
	s_speedup_requested = true;
}

#else   // host

void hwInit(void)
{
	if (s_asked) return;
	s_asked = true;
	// s_new_3ds is whatever hwTestSetNew3ds left; no console to ask and no clock to change.
	s_speedup_requested = false;
}

void hwTestSetNew3ds(bool new_3ds) { s_new_3ds = new_3ds; }

void hwTestReset(void)
{
	s_asked = false;
	s_new_3ds = false;
	s_speedup_requested = false;
}

#endif
