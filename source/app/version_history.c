#include "app/version_history.h"

// Pure C, nothing else — see app/version_history.h for why, and for what
// tests/version_history_test.c is therefore able to prove about the code that actually ships.

int versionHistoryCount(void)
{
	return VERSION_HISTORY_COUNT;
}

const char* versionHistoryVersionAt(int index)
{
	if (index < 0 || index >= VERSION_HISTORY_COUNT) return "";
	return VERSION_HISTORY[index].version;
}

void versionHistoryNotesAt(int index, WhatsNew* out)
{
	if (!out) return;

	if (index < 0 || index >= VERSION_HISTORY_COUNT) {
		whatsnewClear(out);
		return;
	}

	const VersionHistoryEntry* e = &VERSION_HISTORY[index];
	whatsnewParse((const char*)e->data, (size_t)e->len, out);
}
