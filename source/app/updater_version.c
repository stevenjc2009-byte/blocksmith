#include "app/updater_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void updaterVersionParse(const char* text, int out[3])
{
	out[0] = out[1] = out[2] = 0;
	if (!text) return;

	while (*text == 'v' || *text == 'V' || *text == ' ') text++;

	for (int i = 0; i < 3 && *text; i++)
	{
		char* end = NULL;
		long value = strtol(text, &end, 10);
		if (end == text) break;

		out[i] = (int)value;
		text = end;
		if (*text != '.') break;
		text++;
	}
}

bool updaterVersionIsNewer(const char* candidate, const char* current)
{
	int a[3], b[3];
	updaterVersionParse(candidate, a);
	updaterVersionParse(current, b);

	for (int i = 0; i < 3; i++)
	{
		if (a[i] != b[i]) return a[i] > b[i];
	}
	return false;
}

bool updaterTagFromRedirect(const char* url, char* out, size_t out_size)
{
	static const char* const mark = "/releases/tag/";
	if (!url || !out || out_size == 0) return false;

	const char* found = strstr(url, mark);
	if (!found) return false;

	found += strlen(mark);
	if (*found == '\0') return false;

	snprintf(out, out_size, "%s", found);
	return true;
}

void updaterBuildAssetName(const char* tag, char* out, size_t out_size)
{
	if (!tag || !out || out_size == 0) return;

	const char* number = (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;
	snprintf(out, out_size, "blocksmith%s.cia", number);
}
