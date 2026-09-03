#include "app/updater.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <mbedtls/x509_crt.h>

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

// v1.8.18. The whole CA bundle, read once into a heap buffer while updaterInit() has RomFs
// definitely mounted, and handed to curl as a blob instead of a path from then on. See the
// comment above loadCertBundle() for why.
static void*  s_caBundleData;
static size_t s_caBundleLen;

// v1.8.19. What probeCertBundle() found when it parsed s_caBundleData itself at boot, purely
// to measure. s_caParseRet is mbedtls_x509_crt_parse()'s raw return: 0 all 121 parsed clean,
// >0 that many failed while the rest made it into the chain, <0 an mbedtls error code (a
// file-I/O-family code here would be impossible -- this parses memory, not a path -- so a
// negative reading here can only be MBEDTLS_ERR_X509_ALLOC_FAILED-family exhaustion or the
// content itself). s_caParseCount is the chain length actually walked. Both start at a
// sentinel that cannot be confused with a real mbedtls return, so a failure screen that
// somehow renders before updaterInit() has run is recognisable as exactly that rather than
// read as "0 certs, parse succeeded".
#define CA_PROBE_NOT_RUN (1)
static int s_caParseRet   = CA_PROBE_NOT_RUN;
static int s_caParseCount = -1;

// v1.8.19. curl's own text for the last failed perform() on any handle, captured via
// CURLOPT_ERRORBUFFER in applyCommonOptions(). Often more specific than
// curl_easy_strerror(result) for a TLS/cert failure -- it is the only other thing this build
// can put on the failure screen, since BS_BOTTOM_UI compiles the text console out entirely.
static char s_curlError[CURL_ERROR_SIZE];

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

// The CA bundle's primary home: the RomFs copy baked into this exact build. Kept as a
// named constant rather than a literal in applyCommonOptions() because
// performWithCertFallback() below needs to be able to set curl back to it after a fallback
// attempt, on the same handle, and a typo'd second copy of the string would silently break
// that.
#define CERT_BUNDLE_ROMFS "romfs:/cacert.pem"

// v1.8.17. Second place to look for the same trust bytes when the RomFs copy can't be
// opened or parsed on a given console. This is not a made-up path: it is the CAINFO
// devkitPro's own 3ds-curl 8.4.0 package is built with by default
// (--with-ca-bundle=sdmc:/config/ssl/cacert.pem), and the location several established
// homebrew updaters (Universal-Updater among them) already populate and rely on. Nothing
// about trust changes by trying it - it is asked to vouch for exactly the same Mozilla
// bundle, just read off the SD card's ordinary FAT filesystem instead of RomFs.
#define CERT_BUNDLE_SDMC_FALLBACK "sdmc:/config/ssl/cacert.pem"

// v1.8.18. steve's report, second look: CURLE_SSL_CACERT_BADFILE (77) on real hardware means
// curl could not open or parse whatever CURLOPT_CAINFO named AT THE INSTANT
// curl_easy_perform() ran the TLS handshake -- libcurl opens that file lazily, during
// perform(), not when the option is set. Every romfsInit()/romfsExit() call site in the tree
// was mapped (source/app/updater.c and source/audio/audio.c are the only two; see the
// v1.8.18 vault log for the full list) and updaterInit() below holds RomFs mounted
// continuously from boot to updaterExit(), which runs after the worker thread is joined -- so
// on the call graph as it exists today, nothing can unmount "romfs" out from under a transfer
// in flight. That rules out an unmount race as the cause here specifically.
//
// It does not rule out every other way a lazy file read can fail on this device, and the
// fix below removes the entire class regardless of which exact one it was: CURLOPT_CAINFO_BLOB
// (curl 8.4.0, this portlib's own curl/easy.h has it -- CURLOPTTYPE_BLOB 309) hands curl the
// trust bytes directly, so nothing opens romfs:/cacert.pem, or any file, at perform() time at
// all. It is loaded exactly once, up front, in updaterInit(), while this function's own
// romfsInit() a few lines above is definitely still in effect.
static bool loadCertBundle(void);

// Everything both requests need set the same way. Split out so the API call and the
// download cannot drift apart on the settings that matter - particularly the certificate
// bundle and redirect following, either of which silently breaks the updater if only one
// request has it.
static void applyCommonOptions(CURL* curl, const char* url)
{
	curl_easy_setopt(curl, CURLOPT_URL, url);

	// v1.8.19. Cleared per handle, before anything can write into it, so a failure screen can
	// never show a previous request's text under a different one's curl code. curl only ever
	// writes this on an actual error, so an empty string here after a failed perform() is
	// itself informative -- it means curl had nothing more specific to add beyond the code.
	s_curlError[0] = '\0';
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, s_curlError);

	// GitHub serves release assets as a redirect to a separate host, and it has used
	// relative Location headers in the past. Without this the download is a zero-byte file
	// that installs as a corrupt title.
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);

	// The API rejects requests with no User-Agent outright.
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "blocksmith-3ds/" BLOCKSMITH_VERSION);

	// The whole reason RomFs exists in this project, handed over as an in-memory blob rather
	// than a path -- see the comment above loadCertBundle() declaration for why. NOCOPY is
	// safe here specifically because s_caBundleData is freed nowhere but updaterExit(), and
	// updaterExit() joins the worker thread before it gets there (see reapWorker()), so no
	// transfer using this handle can still be alive when the buffer goes away.
	// performWithCertFallback() below is what happens on top of this if a console still can't
	// use the RomFs copy for some other reason.
	struct curl_blob caBlob;
	caBlob.data  = s_caBundleData;
	caBlob.len   = s_caBundleLen;
	caBlob.flags = CURL_BLOB_NOCOPY;
	curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &caBlob);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

	// A handheld on hotel wifi should give up and say so, not hang the front end until the
	// battery goes.
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 64L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 45L);
}

// v1.8.17. steve's report: on real New 3DS hardware, every single check and every single
// download failed with "Could not reach GitHub: Problem with the SSL CA cert (path? access
// rights?)" - curl_easy_strerror(CURLE_SSL_CACERT_BADFILE), meaning curl could not open or
// parse the file CURLOPT_CAINFO named, not that a certificate was presented and rejected.
//
// Checked directly against the shipped v1.8.16 .cia (not the git tree - see the vault log
// for the byte offsets): romfs:/cacert.pem is embedded whole and correct in that exact
// release, all 121 certificates intact, dated three weeks old. And the same failure hit two
// requests that share nothing but this one option - the unauthenticated /releases/latest
// redirect probe and the api.github.com fallback, different hosts, same CAINFO. So the
// broken link is local to loading romfs:/cacert.pem on that particular console, not
// anything about GitHub's certificate or this project's bundle. Nobody has hardware to
// single out which step of "open romfs:/cacert.pem, size it, read it whole" is failing
// there, so this does not guess at one; it gives curl a second, independently-established
// place to find the identical trusted bytes and retries once before giving up.
//
// Deliberately narrow: only CURLE_SSL_CACERT_BADFILE (77, "could not load CACERT file")
// retries. CURLE_PEER_FAILED_VERIFICATION (60, aliased from the old CURLE_SSL_CACERT name)
// is left alone on purpose - that is curl saying a certificate WAS read and did NOT check
// out, which is a real trust decision this must not paper over by quietly trying a
// different bundle. CURLOPT_SSL_VERIFYPEER/VERIFYHOST are never touched by either attempt.
static CURLcode performWithCertFallback(CURL* curl)
{
	CURLcode result = curl_easy_perform(curl);
	if (result != CURLE_SSL_CACERT_BADFILE) return result;

	// v1.8.18: applyCommonOptions() now hands curl the primary bundle as a blob
	// (CURLOPT_CAINFO_BLOB), not a path, so CURLE_SSL_CACERT_BADFILE reaching this point can
	// no longer be the RomFs file failing to open -- there is no file access on that path any
	// more. It is left in place regardless, narrow and harmless, in case some future console
	// or curl backend still surfaces this exact code for a different reason. Blob and path
	// are mutually exclusive per handle, so the blob is explicitly cleared before the path is
	// set, rather than relying on undocumented precedence between the two options.
	curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, NULL);
	curl_easy_setopt(curl, CURLOPT_CAINFO, CERT_BUNDLE_SDMC_FALLBACK);
	CURLcode retry = curl_easy_perform(curl);

	// Leave the handle the way applyCommonOptions() set it up, in case anything above this
	// function ever reuses the handle for a second request - true of none of today's
	// callers, but nothing here should depend on that staying true.
	curl_easy_setopt(curl, CURLOPT_CAINFO, NULL);
	struct curl_blob caBlob;
	caBlob.data  = s_caBundleData;
	caBlob.len   = s_caBundleLen;
	caBlob.flags = CURL_BLOB_NOCOPY;
	curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &caBlob);
	return retry;
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

	const CURLcode result = performWithCertFallback(curl);
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

	CURLcode result = performWithCertFallback(curl);
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

	CURLcode result = performWithCertFallback(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

	if (headers) curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (result != CURLE_OK)
	{
		// The curl code is deliberate, not decoration: this build has no text console
		// (BS_BOTTOM_UI defaults to 1), so this on-screen line is the only diagnostic that
		// ever leaves the console. The bare code alone is what read as "problem with the
		// SSLCassert" in steve's report before this. v1.8.19 adds the other two numbers
		// alongside it: "ca parse" is probeCertBundle()'s raw mbedtls_x509_crt_parse() return
		// from boot (0 = clean, >0 = that many certs failed while others made it in, <0 = an
		// mbedtls error code -- alloc-failure and content-malformed read differently from
		// each other here), and "certs" is how many ended up in that boot-time chain out of
		// the 121 the bundle should hold. curl's own ERRORBUFFER text is appended in
		// parentheses when it has anything to add beyond the bare code.
		// %.48s on the errorbuffer text, not %s: it is a CURL_ERROR_SIZE (256-byte) buffer and
		// this is a fixed-size stack buffer, and a 400-pixel screen has no use for the whole
		// thing regardless -- see the coordinator's "keep it terse" instruction in the
		// v1.8.19 vault log. The precision also lets the compiler prove this snprintf cannot
		// truncate, which an unbounded %s here cannot (-Wformat-truncation, fatal under this
		// project's -Werror).
		char reason[192];
		snprintf(reason, sizeof(reason), "Could not reach GitHub: curl %d / ca parse %d / certs %d%s%.48s",
		         (int)result, s_caParseRet, s_caParseCount,
		         s_curlError[0] ? " - " : "", s_curlError);
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

	CURLcode result = performWithCertFallback(curl);
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
			// See the matching comment in runCheck()'s CURLE_OK branch for what the three
			// numbers mean -- curl code, probeCertBundle()'s boot-time mbedtls_x509_crt_parse()
			// return, and the certificate count that parse produced -- and for why the
			// errorbuffer text is capped with %.48s rather than %s.
			snprintf(reason, sizeof(reason), "Download failed: curl %d / ca parse %d / certs %d%s%.48s",
			         (int)result, s_caParseRet, s_caParseCount,
			         s_curlError[0] ? " - " : "", s_curlError);
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

// Reads romfs:/cacert.pem whole into a heap buffer, growing it as needed rather than sizing
// it with fseek(SEEK_END)/ftell() first. Two independent reasons, both already established
// elsewhere in this codebase rather than new ones invented here:
//
//  - audio.c's readWhole() (source/audio/audio.c, right above romfsHoldBegin) makes the same
//    choice for the same "romfs:" prefix, on the grounds that a devoptab reporting a length
//    it then cannot deliver is a real failure mode on this platform (a truncated or
//    bit-rotted RomFs region inside the installed .cia would still answer a stat/seek with
//    its recorded size), and the read itself is the only thing that can be trusted. This
//    file's own cert bundle deserves the same treatment its neighbour already gives sound
//    assets loaded from the exact same "romfs:" device.
//  - romfs_seek was disassembled directly out of this project's libctru.a (2.7.0-1;
//    C:/devkitPro/libctru/lib/libctru.a, romfs_dev.o) to confirm SEEK_END is even implemented
//    -- it is -- so this is belt-and-suspenders on top of a working primitive, not a
//    workaround for a broken one.
//
// Called exactly once, from updaterInit(), while its own romfsInit() a few lines below is
// definitely still in effect.
#define CA_BUNDLE_INITIAL_CAP  (128 * 1024)
#define CA_BUNDLE_HARD_CEILING (2 * 1024 * 1024)

static bool loadCertBundle(void)
{
	size_t cap = CA_BUNDLE_INITIAL_CAP;
	unsigned char* buf = (unsigned char*)malloc(cap);
	if (!buf) return false;

	FILE* f = fopen(CERT_BUNDLE_ROMFS, "rb");
	if (!f) { free(buf); return false; }

	size_t total = 0;
	for (;;)
	{
		if (total == cap)
		{
			if (cap >= CA_BUNDLE_HARD_CEILING) { fclose(f); free(buf); return false; }
			size_t newCap = cap * 2;
			if (newCap > CA_BUNDLE_HARD_CEILING) newCap = CA_BUNDLE_HARD_CEILING;
			unsigned char* grown = (unsigned char*)realloc(buf, newCap);
			if (!grown) { fclose(f); free(buf); return false; }
			buf = grown;
			cap = newCap;
		}

		size_t n = fread(buf + total, 1, cap - total, f);
		total += n;
		if (n == 0) break;
	}
	fclose(f);

	if (total == 0) { free(buf); return false; }

	// mbedtls_x509_crt_parse() -- both curl's CURLOPT_CAINFO_BLOB path and this file's own
	// probeCertBundle() below -- tells PEM text apart from raw DER by checking buf[buflen-1]
	// == '\0'; without that, and buflen counting it, PEM data is silently sent down the DER
	// parser and fails. The loop above always leaves at least one spare byte in buf (it grows
	// BEFORE the read that would otherwise exactly fill it, so cap > total on every successful
	// exit), so this is always safe within the current allocation.
	buf[total] = '\0';

	// Shrink to the size actually read, plus the terminator. A live-block shrink practically
	// never fails; if it somehow does, the original (larger) allocation is still valid and
	// still holds the same bytes -- including the NUL just written above -- so it is used
	// as-is rather than treating this as a failure.
	unsigned char* fit = (unsigned char*)realloc(buf, total + 1);
	s_caBundleData = fit ? fit : buf;
	s_caBundleLen  = total + 1; // counts the trailing NUL -- see CURLOPT_CAINFO_BLOB's docs
	return true;
}

// v1.8.19. Parses s_caBundleData with our own throwaway mbedtls_x509_crt chain, once, at
// boot -- see the comment on s_caParseRet above for what the two numbers mean. This exists
// because curl 77 (CURLE_SSL_CACERT_BADFILE) is what mbedtls.c returns for ANY negative
// result from mbedtls_x509_crt_parse(), and that one code covers three different failures
// that all look identical from curl's side: the file could not be opened (already
// impossible for the blob path, since there is no file access at all), the allocation burst
// building 121 chained mbedtls_x509_crt nodes ran out of heap, or the content itself is
// malformed. Reasoning cannot tell those apart from here; this measures it directly, on the
// exact bytes curl will use, before any network access happens.
static void probeCertBundle(void)
{
	mbedtls_x509_crt chain;
	mbedtls_x509_crt_init(&chain);

	s_caParseRet = mbedtls_x509_crt_parse(&chain, (const unsigned char*)s_caBundleData,
	                                       s_caBundleLen);

	// Per mbedtls's own contract: a negative return means every certificate in the buffer
	// failed and the chain was never populated, so counting is skipped rather than walking a
	// head node that mbedtls_x509_crt_init() only zeroed and never filled in. A return >= 0
	// means the chain holds exactly the certificates that parsed, so it is walked as-is.
	s_caParseCount = 0;
	if (s_caParseRet >= 0)
	{
		for (mbedtls_x509_crt* cur = &chain; cur != NULL; cur = cur->next)
			s_caParseCount++;
	}

	// Never kept -- this is a measurement, not a trust store the updater goes on to use. See
	// task item 5 in the v1.8.19 vault log for the follow-up this opens (parsing once here and
	// handing curl the already-built chain instead of re-parsing per request), which is a
	// separate change and not part of this one.
	mbedtls_x509_crt_free(&chain);
}

bool updaterInit(bool soc_already_up)
{
	// RomFs carries the certificate bundle. Without it every HTTPS request fails, so there
	// is no point bringing the rest up.
	if (R_FAILED(romfsInit())) return false;
	s_romfsUp = true;

	// v1.8.18: read the bundle into memory right here, before anything else, while this
	// romfsInit() above is guaranteed to still be in effect. If this fails the updater cannot
	// possibly work -- there is no fallback for the primary bundle not existing at all, only
	// for it not being loadable by curl at perform() time -- so this gives up exactly the way
	// the SOC/AM failure branches below already do.
	if (!loadCertBundle())
	{
		romfsExit();
		s_romfsUp = false;
		return false;
	}

	// v1.8.19. Measurement only -- see probeCertBundle()'s own comment. Never itself a reason
	// to fail updaterInit(): a bad reading here is exactly the thing the failure screen needs
	// to be able to show, which requires the updater to still come up and attempt a real
	// request rather than being greyed out before it can be observed at all.
	probeCertBundle();

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
			if (s_caBundleData) { free(s_caBundleData); s_caBundleData = NULL; s_caBundleLen = 0; }
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
		if (s_caBundleData) { free(s_caBundleData); s_caBundleData = NULL; s_caBundleLen = 0; }
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

	// reapWorker() above already joined the worker thread, so no CURL handle holding a
	// CURLOPT_CAINFO_BLOB NOCOPY pointer into this buffer can still be alive -- safe to free
	// before dropping the RomFs mount it was originally read from.
	if (s_caBundleData) { free(s_caBundleData); s_caBundleData = NULL; s_caBundleLen = 0; }

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
	if (!s_available) return;

	// v1.8.4 widens this by exactly one state. UPDATE_AVAILABLE is the ordinary route in;
	// a failed download is the new one, and it is safe for the same reason the ordinary
	// route is: runInstall() reads s_assetUrl, runCheck() is the only thing that writes it,
	// and nothing on any failure path clears it. So the URL a retry uses is the URL the
	// successful check produced, byte for byte.
	//
	// The half-written title the retry would otherwise land on top of is already handled:
	// every failure inside runInstall() calls AM_CancelCIAInstall on the way out (see the
	// comment above that block), so a retry starts a genuinely fresh install handle rather
	// than resuming a broken one.
	const bool retrying_download = (s_state == UPDATE_FAILED) && (s_job == JOB_INSTALL) &&
	                               (s_assetUrl[0] != '\0');
	if (s_state != UPDATE_AVAILABLE && !retrying_download) return;

	startWorker(JOB_INSTALL);
}

bool updaterDownloadFailed(void)
{
	// s_job is written by startWorker() on the main thread BEFORE the worker exists, so it
	// is already correct by the time any state the worker sets becomes visible. That is the
	// reason this reads s_job rather than a flag set alongside s_state = UPDATE_FAILED: a
	// flag written after the state would break the ordering rule this file runs on (fill the
	// data, then move the state) and could be read stale for a frame.
	return (s_state == UPDATE_FAILED) && (s_job == JOB_INSTALL) && (s_assetUrl[0] != '\0');
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
