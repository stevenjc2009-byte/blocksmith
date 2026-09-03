// See stage_probe.h for what this is and why every write is raw-FS and append-only.
#include "app/stage_probe.h"

#if BS_STAGE_PROBE

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#define SP_DIR_REL  "/blocksmith"
#define SP_FILE_REL "/blocksmith/stage.txt"

static volatile u32  s_seq;
static volatile bool s_once_fired[BS_STAGE_ID_COUNT];

// Appends `text` (already formatted, `len` bytes) to stage.txt. Never truncates — unlike
// app/watchdog.c's fileWrite, which sizes the file to exactly one report every time, this one
// must never destroy an earlier marker, because the earlier markers are the whole record.
//
// The three lines that matter, in order:
//   1. FSFILE_GetSize      — read the CURRENT end of file. The 3DS FS service has no "append"
//                            open flag, so this is how an append is built: open for write,
//                            find out how long the file already is, write starting there.
//   2. FSFILE_Write(..., FS_WRITE_FLUSH | FS_WRITE_UPDATE_TIME) — FS_WRITE_FLUSH makes this
//                            call itself synchronous: it does not return until the bytes are
//                            committed to the SD card, not merely handed to a driver buffer.
//   3. FSFILE_Close        — closes the handle before this function returns.
//
// Together these are the entire point of the probe: by the time bsStageMark's caller moves on
// to the next stage, this marker is already on the card. A freeze one instruction later still
// leaves this line as the last one in the file.
static void spAppend(const char* text, size_t len)
{
	FS_Archive archive;
	Result rc = FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
	if (R_FAILED(rc)) return;

	FSUSER_CreateDirectory(archive, fsMakePath(PATH_ASCII, SP_DIR_REL), FS_ATTRIBUTE_DIRECTORY);

	Handle file;
	rc = FSUSER_OpenFile(&file, archive, fsMakePath(PATH_ASCII, SP_FILE_REL),
	                     FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
	if (R_SUCCEEDED(rc)) {
		u64 size = 0;
		FSFILE_GetSize(file, &size);

		u32 written = 0;
		FSFILE_Write(file, &written, size, text, (u32)len,
		             FS_WRITE_FLUSH | FS_WRITE_UPDATE_TIME);
		FSFILE_Close(file);
	}

	FSUSER_CloseArchive(archive);
}

void bsStageInit(void)
{
	// Truncate to empty via create + SetSize(0), so a file left over from a PREVIOUS boot can
	// never be misread as part of this boot's history. Same open dance as spAppend because
	// this can run before anything else in the process has created sdmc:/blocksmith/.
	FS_Archive archive;
	Result rc = FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
	if (R_SUCCEEDED(rc)) {
		FSUSER_CreateDirectory(archive, fsMakePath(PATH_ASCII, SP_DIR_REL),
		                        FS_ATTRIBUTE_DIRECTORY);

		Handle file;
		rc = FSUSER_OpenFile(&file, archive, fsMakePath(PATH_ASCII, SP_FILE_REL),
		                     FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
		if (R_SUCCEEDED(rc)) {
			FSFILE_SetSize(file, 0);
			FSFILE_Close(file);
		}

		FSUSER_CloseArchive(archive);
	}

	s_seq = 0;
	memset((void*)s_once_fired, 0, sizeof(s_once_fired));
}

void bsStageMark(const char* label)
{
	char buf[128];
	const u32 seq = ++s_seq;   // pre-increment: the first marker written reads seq 1
	const int n = snprintf(buf, sizeof(buf), "%lu %llu %s\n",
	                        (unsigned long)seq,
	                        (unsigned long long)svcGetSystemTick(), label);
	if (n > 0) {
		const size_t len = ((size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
		spAppend(buf, len);
	}
}

void bsStageOnce(BsStageId id, const char* label)
{
	if ((unsigned)id >= (unsigned)BS_STAGE_ID_COUNT) return;
	// Best-effort, not atomic — see stage_probe.h. A worst-case race between two worker lanes
	// double-writes the same marker once; it never loses one, and it never blocks the caller.
	if (s_once_fired[id]) return;
	s_once_fired[id] = true;
	bsStageMark(label);
}

#endif // BS_STAGE_PROBE
