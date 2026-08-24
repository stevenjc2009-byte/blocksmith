#include "app/updater.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "app/updater_version.h"
#include "app/whatsnew.h"

// The socket buffer libctru hands to the network stack. It has to be aligned to 0x1000 and
// a multiple of 0x1000, and 1MB is the size every devkitPro socket example is written
// against. Smaller almost certainly works for one connection at a time, but "almost
// certainly" is not worth debugging over wifi on a handheld, so this stays at the
// well-trodden value. Copied from the sibling project's SOC_BUFFER_SIZE.
#define SOC_BUFFER_SIZE (0x100000)

// The worker's stack. An mbedtls handshake is not shy with stack, so this is deliberately
// roomier than the 32KB a plain worker would get.
#define WORKER_STACK_SIZE (0x10000)

// Ceiling on the GitHub API response held in memory. A releases payload is a few KB;
// anything past this is a sign something other than the API answered, and truncating beats
// growing a buffer until the heap gives out.
#define API_RESPONSE_MAX (128 * 1024)

// What the worker was asked to do. The thread body switches on this once.
typedef enum
{
	JOB_CHECK,
	JOB_INSTALL,
} updateJob;

static bool  s_available;      ///< Did updaterInit get its services up.
static u32*  s_socBuf;
static bool  s_socUp, s_amUp, s_romfsUp;

static Thread s_worker;
static bool   s_workerLive;    ///< A thread exists and has not been reaped.
static updateJob s_job;

// Written by the worker, read by the main thread every frame.
//
// The ordering rule that makes this safe without a mutex: the worker fills in s_msg,
// s_latest, s_assetUrl and s_progress *before* it moves s_state to the value that makes
// them meaningful. The main thread reads s_state first and only then touches the rest. A
// word-sized store on ARM11 will not tear, so the worst a badly timed frame can do is paint
// one frame of stale progress.
static volatile updateState s_state = UPDATE_IDLE;
static volatile int  s_progress = -1;
static char s_msg[192];
static char s_latest[32];
static char s_assetUrl[512];

// The release notes shown on the top screen before the player commits to the download
// (v1.6.0 task 14b). Written by the worker inside runCheck(), *before* it moves s_state to
// UPDATE_AVAILABLE, so it obeys the same ordering rule as everything above it: the main
// thread reads s_state first and only then touches this.
//
// Static rather than heap for the reason app/whatsnew.h gives: nothing about a file arriving
// off the network should be able to make a 12 MB console reserve memory. ~3 KB of .bss.
static WhatsNew s_notes;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static void setMessage(const char* text)
{
	snprintf(s_msg, sizeof(s_msg), "%s", text ? text : "");
}

// Copies the JSON string value that starts at *cursor* - which must point at the opening
// quote - into out, undoing the two escapes GitHub actually emits in these fields. Returns
// the character after the closing quote, or NULL.
static const char* jsonCopyString(const char* cursor, char* out, size_t outSize)
{
	if (!cursor || *cursor != '"' || outSize == 0) return NULL;
	cursor++;

	size_t written = 0;
	while (*cursor && *cursor != '"')
	{
		char c = *cursor++;
		if (c == '\\' && *cursor)
		{
			char escaped = *cursor++;
			c = (escaped == 'n') ? '\n' : (escaped == 't') ? '\t' : escaped;
		}
		if (written + 1 < outSize) out[written++] = c;
	}

	out[written] = '\0';
	return (*cursor == '"') ? cursor + 1 : NULL;
}

// Finds "key": "value" and copies the value out. Deliberately naive - it does not
// understand nesting - which is fine because the two fields wanted here are unambiguous in
// a releases payload.
static const char* jsonFindValue(const char* json, const char* key)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);

	const char* at = strstr(json, pattern);
	if (!at) return NULL;

	at += strlen(pattern);
	while (*at == ' ' || *at == ':' || *at == '\t' || *at == '\n' || *at == '\r') at++;
	return (*at == '"') ? at : NULL;
}

static bool jsonGetString(const char* json, const char* key, char* out, size_t outSize)
{
	const char* at = jsonFindValue(json, key);
	return at && jsonCopyString(at, out, outSize) != NULL;
}

// Walks every browser_download_url in the payload and keeps the first that ends in ".cia".
// A release can legitimately carry a .3dsx and a source zip too, and picking whichever came
// first would install the wrong thing.
static bool jsonFindCiaAsset(const char* json, char* out, size_t outSize)
{
	static const char* const key = "\"browser_download_url\"";
	const char* scan = json;

	while ((scan = strstr(scan, key)) != NULL)
	{
		scan += strlen(key);
		while (*scan == ' ' || *scan == ':') scan++;

		char candidate[512];
		const char* after = jsonCopyString(scan, candidate, sizeof(candidate));
		if (!after) break;
		scan = after;

		size_t length = strlen(candidate);
		if (length > 4 && strcmp(candidate + length - 4, ".cia") == 0)
		{
			snprintf(out, outSize, "%s", candidate);
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// curl plumbing
// ---------------------------------------------------------------------------

typedef struct
{
	char*  data;
	size_t length;
} memoryBuffer;

static size_t writeToMemory(char* chunk, size_t size, size_t count, void* userData)
{
	memoryBuffer* buffer = (memoryBuffer*)userData;
	size_t incoming = size * count;

	if (buffer->length + incoming > API_RESPONSE_MAX) return 0;

	char* grown = realloc(buffer->data, buffer->length + incoming + 1);
	if (!grown) return 0;

	buffer->data = grown;
	memcpy(buffer->data + buffer->length, chunk, incoming);
	buffer->length += incoming;
	buffer->data[buffer->length] = '\0';
	return incoming;
}

// The far end of the download: bytes go straight into the AM install handle rather than to
// a file on the SD card. Nothing is staged, so a 200KB update needs no free space and there
// is no half-written .cia to clean up.
typedef struct
{
	Handle handle;
	u64    offset;
	bool   failed;
} installSink;

static size_t writeToInstall(char* chunk, size_t size, size_t count, void* userData)
{
	installSink* sink = (installSink*)userData;
	size_t incoming = size * count;

	u32 written = 0;
	if (R_FAILED(FSFILE_Write(sink->handle, &written, sink->offset, chunk, (u32)incoming, 0)))
	{
		sink->failed = true;
		return 0;
	}

	sink->offset += written;
	return written;
}

static int reportProgress(void* userData, curl_off_t total, curl_off_t now,
                          curl_off_t upTotal, curl_off_t upNow)
{
	(void)userData; (void)upTotal; (void)upNow;

	if (total > 0)
	{
		s_progress = (int)((now * 100) / total);
	}
	return 0;
}

// Everything both requests need set the same way. Split out so the API call and the
// download cannot drift apart on the settings that matter - particularly the certificate
// bundle and redirect following, either of which silently breaks the updater if only one
// request has it.
static void applyCommonOptions(CURL* curl, const char* url)
{
	curl_easy_setopt(curl, CURLOPT_URL, url);

	// GitHub serves release assets as a redirect to a separate host, and it has used
	// relative Location headers in the past. Without this the download is a zero-byte file
	// that installs as a corrupt title.
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);

	// The API rejects requests with no User-Agent outright.
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "blocksmith-3ds/" BLOCKSMITH_VERSION);

	// The whole reason RomFs exists in this project.
	curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

	// A handheld on hotel wifi should give up and say so, not hang the front end until the
	// battery goes.
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 64L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 45L);
}

// ---------------------------------------------------------------------------
// Asking which release is current without touching the JSON API
// ---------------------------------------------------------------------------
//
// api.github.com allows sixty unauthenticated requests an hour per IP address and answers
// 403 to the sixty-first. That allowance is shared by everything on the same connection, so
// an update check can fail for reasons that have nothing to do with this console and then
// work an hour later on its own - the "GitHub answered with HTTP 403" report, which
// retrying cannot help with.
//
// The plain release page has no such ceiling. /releases/latest answers 302 with the tag in
// its Location header, which is the only thing the check needs, so that is asked first and
// the API is kept as the fallback.

// Builds the download URL from the tag rather than from a fixed filename, because every
// release names its CIA after its own version - blocksmith0.1.0.cia at tag v0.1.0.
//
// The tag form is used in preference to /releases/latest/download/ so that a release
// published between the check and the download cannot quietly hand over a different
// version than the one the player was shown.
static void buildAssetUrl(const char* tag, char* out, size_t outSize)
{
	char assetName[64];
	updaterBuildAssetName(tag, assetName, sizeof(assetName));
	snprintf(out, outSize, "https://github.com/%s/%s/releases/download/%s/%s",
	         BLOCKSMITH_REPO_OWNER, BLOCKSMITH_REPO_NAME, tag, assetName);
}

// ---------------------------------------------------------------------------
// The release notes shown before the download (v1.6.0 task 14b)
// ---------------------------------------------------------------------------
//
// Same predictable path the .cia comes from, a tag away: whatsnew1.6.0.txt sits next to
// blocksmith1.6.0.cia on the release. Deliberately NOT api.github.com and deliberately not
// the release body — see the block comment above checkViaRedirect for what the sixty-an-hour
// ceiling did to this updater once already, and app/whatsnew.h for the rest of the reasoning.
//
// Every failure here is soft, without exception. A release with no notes asset (which is
// every release published before this feature existed), a 404, a timeout, a TLS failure, a
// file of binary garbage: all of them leave s_notes empty, the screen draws
// whatsnewPlaceholder() and the download is offered exactly as it was before. There is no
// error dialog and no path where missing notes can block an update.

// Past this many bytes the transfer is abandoned rather than followed wherever it leads.
// Four times the keep-cap: enough slack that a slightly over-long file is still read to its
// natural end, small enough that a hostile one cannot stream at this console indefinitely.
#define NOTES_HARD_CEILING (4 * WHATSNEW_BYTES_MAX)

typedef struct
{
	char   data[WHATSNEW_BYTES_MAX + 1];
	size_t length;   ///< bytes actually kept
	size_t seen;     ///< bytes curl has offered, kept or not
} notesSink;

// File-scope rather than a local: 4 KB is not something to put on a worker stack that an
// mbedtls handshake is already sharing. Only the worker thread touches it.
static notesSink s_notesSink;

static size_t writeNotes(char* chunk, size_t size, size_t count, void* userData)
{
	notesSink* sink = (notesSink*)userData;
	const size_t incoming = size * count;

	if (sink->length < WHATSNEW_BYTES_MAX)
	{
		const size_t room = WHATSNEW_BYTES_MAX - sink->length;
		const size_t take = (incoming < room) ? incoming : room;
		memcpy(sink->data + sink->length, chunk, take);
		sink->length += take;
		sink->data[sink->length] = '\0';
	}

	sink->seen += incoming;

	// Returning short of `incoming` is how a curl write callback says stop. The bytes already
	// banked are kept and parsed by the caller: truncated notes beat no notes, and
	// whatsnewParse marks the truncation so the screen says so.
	return (sink->seen > NOTES_HARD_CEILING) ? 0 : incoming;
}

static void fetchReleaseNotes(const char* tag)
{
	whatsnewClear(&s_notes);

	char name[64];
	updaterBuildNotesName(tag, name, sizeof(name));

	char url[512];
	snprintf(url, sizeof(url), "https://github.com/%s/%s/releases/download/%s/%s",
	         BLOCKSMITH_REPO_OWNER, BLOCKSMITH_REPO_NAME, tag, name);

	CURL* curl = curl_easy_init();
	if (!curl) return;

	memset(&s_notesSink, 0, sizeof(s_notesSink));

	applyCommonOptions(curl, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeNotes);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s_notesSink);

	// A hard ceiling on the whole request, which the .cia download deliberately does not have.
	// Nobody should wait on the notes: at worst this delays the "a newer version is available"
	// report by twenty seconds, and the front end is still drawing at 60 Hz throughout because
	// this runs on the worker thread like everything else in this file.
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

	const CURLcode result = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);

	// CURLE_WRITE_ERROR with bytes banked is the size ceiling above firing, not a real
	// failure, so what did arrive is still parsed.
	const bool stopped_for_size = (result == CURLE_WRITE_ERROR && s_notesSink.length > 0);

	if ((result == CURLE_OK || stopped_for_size) && status == 200)
		whatsnewParse(s_notesSink.data, s_notesSink.length, &s_notes);
}

// Returns true only if GitHub redirected and the tag could be read out of it. Anything else
// - no redirect, a transport error, a shape that has changed - falls through to the API,
// which still works; it is only rationed.
static bool checkViaRedirect(char* tag, size_t tagSize)
{
	char url[256];
	snprintf(url, sizeof(url), "https://github.com/%s/%s/releases/latest",
	         BLOCKSMITH_REPO_OWNER, BLOCKSMITH_REPO_NAME);

	CURL* curl = curl_easy_init();
	if (!curl) return false;

	applyCommonOptions(curl, url);

	// The redirect IS the answer here, so it must not be followed, and the page body it
	// would lead to is of no interest.
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);

	CURLcode result = curl_easy_perform(curl);
	long  status   = 0;
	char* redirect = NULL;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redirect);

	// Read before cleanup - redirect points into the handle's own memory.
	bool got = (result == CURLE_OK && status >= 300 && status < 400 &&
	            updaterTagFromRedirect(redirect, tag, tagSize));

	curl_easy_cleanup(curl);
	return got;
}

// ---------------------------------------------------------------------------
// The two jobs
// ---------------------------------------------------------------------------

static void runCheck(void)
{
	s_progress = -1;

	// Cleared up front so a second check that fails cannot leave the previous release's
	// notes on screen next to the new version number.
	whatsnewClear(&s_notes);

	// Nothing to compare against. Said plainly and stopped here, because the alternative -
	// reading a blank version as 0.0.0 - would make every release on GitHub look newer and
	// offer an update that is not one.
	if (!BLOCKSMITH_VERSION_SET)
	{
		setMessage("This build has no version number set, so there is nothing to compare.");
		s_state = UPDATE_FAILED;
		return;
	}

	setMessage("Asking GitHub for the latest release...");
	s_state = UPDATE_CHECKING;

	// The unrationed route first. When it answers there is no API request at all, so a
	// check cannot be spent out of an allowance shared with whatever else is on the same
	// connection.
	char redirectTag[32] = { 0 };
	if (checkViaRedirect(redirectTag, sizeof(redirectTag)))
	{
		snprintf(s_latest, sizeof(s_latest), "%s", redirectTag);

		if (!updaterVersionIsNewer(redirectTag, BLOCKSMITH_VERSION))
		{
			setMessage("This is the newest version.");
			s_state = UPDATE_UP_TO_DATE;
			return;
		}

		buildAssetUrl(redirectTag, s_assetUrl, sizeof(s_assetUrl));

		// Before UPDATE_AVAILABLE, never after: the ordering rule at the top of this file is
		// that the worker finishes writing everything the main thread will read and only then
		// moves the state. The message is updated first so a slow notes fetch says what it is
		// waiting on instead of sitting on "Asking GitHub for the latest release...".
		setMessage("Reading the change notes...");
		fetchReleaseNotes(redirectTag);

		setMessage("A newer version is available.");
		s_state = UPDATE_AVAILABLE;
		return;
	}

	char url[256];
	snprintf(url, sizeof(url),
	         "https://api.github.com/repos/%s/%s/releases/latest",
	         BLOCKSMITH_REPO_OWNER, BLOCKSMITH_REPO_NAME);

	memoryBuffer buffer = { NULL, 0 };
	CURL* curl = curl_easy_init();
	if (!curl)
	{
		setMessage("Could not start the network client.");
		s_state = UPDATE_FAILED;
		return;
	}

	applyCommonOptions(curl, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToMemory);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

	struct curl_slist* headers = curl_slist_append(NULL, "Accept: application/vnd.github+json");
	if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	CURLcode result = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

	if (headers) curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (result != CURLE_OK)
	{
		char reason[192];
		snprintf(reason, sizeof(reason), "Could not reach GitHub: %s",
		         curl_easy_strerror(result));
		setMessage(reason);
		s_state = UPDATE_FAILED;
		free(buffer.data);
		return;
	}

	if (status == 404)
	{
		// The overwhelmingly likely cause, and worth naming precisely: the repo is
		// reachable but has never had a release published, so there is nothing to compare
		// against.
		setMessage("No releases have been published for this game yet.");
		s_state = UPDATE_FAILED;
		free(buffer.data);
		return;
	}

	if (status != 200 || !buffer.data)
	{
		char reason[128];
		snprintf(reason, sizeof(reason), "GitHub answered with HTTP %ld.", status);
		setMessage(reason);
		s_state = UPDATE_FAILED;
		free(buffer.data);
		return;
	}

	char tag[32] = { 0 };
	if (!jsonGetString(buffer.data, "tag_name", tag, sizeof(tag)))
	{
		setMessage("GitHub's answer did not contain a version tag.");
		s_state = UPDATE_FAILED;
		free(buffer.data);
		return;
	}

	if (!updaterVersionIsNewer(tag, BLOCKSMITH_VERSION))
	{
		snprintf(s_latest, sizeof(s_latest), "%s", tag);
		setMessage("This is the newest version.");
		s_state = UPDATE_UP_TO_DATE;
		free(buffer.data);
		return;
	}

	// Prefer the URL GitHub actually gave us; fall back to the stable path that resolves
	// as long as the asset keeps its name. The fallback is what keeps this working if the
	// payload shape ever changes under us.
	if (!jsonFindCiaAsset(buffer.data, s_assetUrl, sizeof(s_assetUrl)))
	{
		snprintf(s_assetUrl, sizeof(s_assetUrl),
		         "https://github.com/%s/%s/releases/latest/download/%s",
		         BLOCKSMITH_REPO_OWNER, BLOCKSMITH_REPO_NAME, BLOCKSMITH_CIA_ASSET);
	}

	free(buffer.data);

	snprintf(s_latest, sizeof(s_latest), "%s", tag);

	// Same two lines as the redirect path above, for the same reason. Both routes reach
	// UPDATE_AVAILABLE, so notes fetched on only one of them would be a changelog that
	// appears or not depending on which one answered.
	setMessage("Reading the change notes...");
	fetchReleaseNotes(tag);

	setMessage("A newer version is available.");
	s_state = UPDATE_AVAILABLE;
}

static void runInstall(void)
{
	s_progress = 0;
	setMessage("Downloading the update...");
	s_state = UPDATE_DOWNLOADING;

	installSink sink = { 0, 0, false };
	if (R_FAILED(AM_StartCiaInstall(MEDIATYPE_SD, &sink.handle)))
	{
		setMessage("The console refused to start the install.");
		s_state = UPDATE_FAILED;
		return;
	}

	CURL* curl = curl_easy_init();
	if (!curl)
	{
		AM_CancelCIAInstall(sink.handle);
		setMessage("Could not start the network client.");
		s_state = UPDATE_FAILED;
		return;
	}

	applyCommonOptions(curl, s_assetUrl);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToInstall);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, reportProgress);

	CURLcode result = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);

	// Every failure below has to cancel the install handle. A handle left open is a
	// half-written title on the SD card that the player then has to delete by hand through
	// Data Management.
	if (result != CURLE_OK || status != 200 || sink.failed || sink.offset == 0)
	{
		AM_CancelCIAInstall(sink.handle);

		char reason[192];
		if (sink.failed)
			snprintf(reason, sizeof(reason), "The console stopped accepting the download.");
		else if (result != CURLE_OK)
			snprintf(reason, sizeof(reason), "Download failed: %s", curl_easy_strerror(result));
		else
			snprintf(reason, sizeof(reason), "Download failed with HTTP %ld.", status);

		setMessage(reason);
		s_state = UPDATE_FAILED;
		return;
	}

	s_progress = 100;
	setMessage("Installing...");
	s_state = UPDATE_INSTALLING;

	if (R_FAILED(AM_FinishCiaInstall(sink.handle)))
	{
		// Nothing to cancel here - Finish consumes the handle either way. The usual cause
		// is an unsigned title on a console without signature patches, which is worth
		// saying plainly rather than as an error code.
		setMessage("The install was rejected. Custom firmware with signature patches is required.");
		s_state = UPDATE_FAILED;
		return;
	}

	setMessage("Update installed.");
	s_state = UPDATE_DONE;
}

static void workerBody(void* argument)
{
	(void)argument;

	if (s_job == JOB_CHECK) runCheck();
	else                    runInstall();
}

// Reaps a finished worker so the next press can start a fresh one. Called from the main
// thread only, and only when the state says the worker has stopped.
static void reapWorker(void)
{
	if (!s_workerLive) return;

	threadJoin(s_worker, U64_MAX);
	threadFree(s_worker);
	s_workerLive = false;
}

static void startWorker(updateJob job)
{
	reapWorker();

	s_job = job;

	s32 priority = 0x30;
	svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);

	// One notch above the main thread. The worker spends nearly all its life blocked on the
	// network, so it costs the front end nothing, and it means a busy frame cannot stall the
	// download.
	priority -= 1;
	if (priority < 0x18) priority = 0x18;
	if (priority > 0x3F) priority = 0x3F;

	s_worker = threadCreate(workerBody, NULL, WORKER_STACK_SIZE, priority, -2, false);
	if (!s_worker)
	{
		setMessage("Could not start the update thread.");
		s_state = UPDATE_FAILED;
		return;
	}
	s_workerLive = true;
}

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

bool updaterInit(bool soc_already_up)
{
	// RomFs carries the certificate bundle. Without it every HTTPS request fails, so there
	// is no point bringing the rest up.
	if (R_FAILED(romfsInit())) return false;
	s_romfsUp = true;

	// Model Kit, which this file is ported from, is the only thing in its process that uses a
	// socket. This game is not: net/bsnet_sock.c calls socInit() from netInit() at the very top
	// of main(), and libctru keeps exactly one SOCU service handle per process — a second
	// socInit() would overwrite it, orphaning the handle the net module's file descriptors were
	// opened against. So SOC is brought up here only when nobody else has it, and the caller is
	// the one that knows. Sharing it is free: sockets are process-wide, and libcurl asks the C
	// library for one like anything else.
	if (!soc_already_up)
	{
		s_socBuf = (u32*)memalign(0x1000, SOC_BUFFER_SIZE);
		if (!s_socBuf)
		{
			romfsExit();
			s_romfsUp = false;
			return false;
		}

		if (R_FAILED(socInit(s_socBuf, SOC_BUFFER_SIZE)))
		{
			free(s_socBuf);
			s_socBuf = NULL;
			romfsExit();
			s_romfsUp = false;
			return false;
		}
		s_socUp = true;
	}

	// am:net is in the RSF's service list, so this is expected to succeed on a CIA build.
	// It will not on a bare 3DSX without elevated services, which is exactly the case the
	// greyed-out button exists for.
	if (R_FAILED(amInit()))
	{
		// Both guarded: when SOC belongs to the net module, tearing it down here would take
		// multiplayer's sockets with it on the way out of a failure that has nothing to do
		// with them.
		if (s_socUp)  { socExit(); s_socUp = false; }
		if (s_socBuf) { free(s_socBuf); s_socBuf = NULL; }
		romfsExit();
		s_romfsUp = false;
		return false;
	}
	s_amUp = true;

	curl_global_init(CURL_GLOBAL_DEFAULT);

	s_available = true;
	s_state = UPDATE_IDLE;
	s_latest[0] = '\0';
	s_assetUrl[0] = '\0';
	setMessage("");
	return true;
}

void updaterExit(void)
{
	reapWorker();

	if (s_available) curl_global_cleanup();

	if (s_amUp)    { amExit();    s_amUp = false; }
	if (s_socUp)   { socExit();   s_socUp = false; }
	if (s_socBuf)  { free(s_socBuf); s_socBuf = NULL; }
	if (s_romfsUp) { romfsExit(); s_romfsUp = false; }

	s_available = false;
}

bool updaterAvailable(void)
{
	return s_available;
}

void updaterStartCheck(void)
{
	if (!s_available) return;

	updateState now = s_state;
	if (now == UPDATE_CHECKING || now == UPDATE_DOWNLOADING ||
	    now == UPDATE_INSTALLING || now == UPDATE_DONE) return;

	startWorker(JOB_CHECK);
}

void updaterStartInstall(void)
{
	if (!s_available || s_state != UPDATE_AVAILABLE) return;

	startWorker(JOB_INSTALL);
}

updateState updaterState(void)
{
	return s_state;
}

bool updaterBusy(void)
{
	updateState now = s_state;
	return now == UPDATE_CHECKING || now == UPDATE_DOWNLOADING || now == UPDATE_INSTALLING;
}

const char* updaterMessage(void)
{
	return s_msg;
}

int updaterProgress(void)
{
	return s_progress;
}

const char* updaterLatestVersion(void)
{
	return s_latest;
}

const WhatsNew* updaterReleaseNotes(void)
{
	return &s_notes;
}

void updaterRelaunch(void)
{
	if (s_state != UPDATE_DONE) return;

	// Sets the target and returns; the jump itself happens when the app exits, so the
	// caller has to fall out of its main loop straight after this. By then the title on the
	// SD card is the new build, which is what comes back.
	aptSetChainloaderToSelf();
}
