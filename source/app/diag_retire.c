#include "app/diag_retire.h"

#include <stdio.h>

bool diagShouldRetire(bool live_exists)
{
	return live_exists;
}

void diagRetire(const char* live, const char* prev)
{
	// Probe rather than assume. This is the fix: the old code went straight to
	// remove(prev) without ever asking whether this boot had anything to put there.
	FILE* probe = fopen(live, "rb");
	bool live_exists = probe != NULL;
	if (probe) fclose(probe);

	if (!diagShouldRetire(live_exists))
		return;

	remove(prev);
	if (rename(live, prev) != 0)
		remove(live);
}
