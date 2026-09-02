#include "world/daynight.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "world/crc32.h"
#include "world/tick.h"

// Nothing here includes <3ds.h> — see daynight.h's file comment for why that matters.

// The cycle length and the simulation rate are two different files' constants and they have to
// agree, or "a 20-minute day" quietly becomes whatever 24,000 ticks happens to be worth. The
// wiki's figure is 20 minutes; TICK_HZ is 20; 24000 / 20 = 1200 seconds = 20 minutes exactly.
_Static_assert(DAY_TICKS / TICK_HZ == 1200,
               "a Minecraft day is 20 minutes; DAY_TICKS and TICK_HZ no longer agree on that");
_Static_assert(DAY_SKYLIGHT_MAX - DAY_SKYDARKEN_MAX == DAY_SKYLIGHT_MIN,
               "midnight sky light is 15 - 11 = 4");

// ── The counter ──────────────────────────────────────────────────────────────────────

void dayNightInit(DayNight* d, uint64_t ticks)
{
	if (!d) return;
	d->ticks = ticks;
}

void dayNightAdvance(DayNight* d, int n)
{
	if (!d) return;

	// A negative count rewinds nothing, for world/tick.h's reason: tickClockAdvance already
	// refuses negative elapsed time, so the only way a negative reaches here is a caller
	// passing something that is not a tick count — and running the day backwards over that
	// would move the sun the wrong way on screen and, once time is on the wire, move it the
	// wrong way for everyone in the room.
	if (n > 0)
		d->ticks += (uint64_t)n;
}

void dayNightSet(DayNight* d, uint64_t ticks)
{
	if (!d) return;
	d->ticks = ticks;
}

uint64_t dayNightTicks(const DayNight* d) { return d ? d->ticks : 0u; }

uint32_t dayNightTimeOfDay(const DayNight* d)
{
	return d ? (uint32_t)(d->ticks % DAY_TICKS) : DAY_START_TICKS;
}

uint64_t dayNightDay(const DayNight* d) { return d ? (d->ticks / DAY_TICKS) : 0u; }

unsigned dayNightMoonPhase(const DayNight* d)
{
	return d ? (unsigned)((d->ticks / DAY_TICKS) % 8u) : 0u;
}

// ── The pure functions ───────────────────────────────────────────────────────────────

// Every entry point below funnels through this. A caller that hands over a whole counter
// instead of a time of day gets the right answer rather than a fault, which matters because
// the two are both uint32-shaped and only one of them is bounded.
static uint32_t wrap(uint32_t tod) { return tod % DAY_TICKS; }

DayPhase dayNightPhase(uint32_t tod)
{
	const uint32_t t = wrap(tod);
	if (t < DAY_TICK_SUNSET)  return DAY_PHASE_DAY;
	if (t < DAY_TICK_NIGHT)   return DAY_PHASE_SUNSET;
	if (t < DAY_TICK_SUNRISE) return DAY_PHASE_NIGHT;
	return DAY_PHASE_SUNRISE;
}

float dayNightCelestialAngle(uint32_t tod)
{
	const float t = (float)wrap(tod);

	// Minecraft's getTimeOfDay, and the wiki's Sky angle formula, which are the same function
	// written two ways. The wiki gives (https://minecraft.wiki/w/Daylight_cycle, "Sky angle"):
	//
	//     alpha = (1 - cos(pi * mod_1((t-6000)/24000)) + mod_4((t-6000)/6000)) * 60 degrees
	//
	// Write d = mod_1((t-6000)/24000). Then (t-6000)/6000 is 4*(t-6000)/24000, so its value
	// mod 4 is 4d, and
	//
	//     alpha = (1 - cos(pi*d) + 4d) * 60 = 60 - 60*cos(pi*d) + 240*d   degrees.
	//
	// Dividing by 360 to get rotations:
	//
	//     alpha/360 = 1/6 - cos(pi*d)/6 + 2d/3 = (0.5 - cos(pi*d)/2)/3 + 2d/3
	//
	// which is the game's `d * 2/3 + (0.5 - cos(d*pi)/2) / 3` line, term for term. The two
	// agreeing is the reason this file trusts either: the wiki's own talk page has an open
	// question about where its formula came from, and it landing exactly on the source
	// function's arithmetic answers it.
	//
	// The -0.25 is the -6000/24000 above. It is what makes 0 rotations noon rather than dawn.
	float d = t * (1.0f / (float)DAY_TICKS) - 0.25f;
	d -= floorf(d);                                  // mod_1, correct for negatives too

	const float e = 0.5f - cosf(d * 3.14159265f) * 0.5f;
	return d * (2.0f / 3.0f) + e * (1.0f / 3.0f);
}

float dayNightSkyDarkenF(uint32_t tod)
{
	// Minecraft's getSkyDarken, without the (int) truncation and without the rain and thunder
	// factors (this project has no weather, and both are 1.0 in clear weather anyway):
	//
	//     f = 0.5 + 2 * clamp(cos(timeOfDay * 2pi), -0.25, 0.25)
	//     skyDarken = (int)((1 - f) * 11)
	//
	// THE CLAMP IS THE WHOLE SHAPE OF THE CYCLE. cos() only lies inside +-0.25 for a narrow
	// window either side of the two horizons, so `f` is pinned at 1.0 through the entire day
	// and at 0.0 through the entire night, and the cosine is live for roughly 1,600 ticks at
	// each end. That is why Minecraft's sky does not fade like a sine from noon to midnight —
	// it sits, drops over about eighty seconds, sits, and comes back.
	//
	// Removing the clamp is what a naive implementation does, and it is measurably wrong: it
	// would put the world at half brightness at 09:00 and at 15:00, which is neither what the
	// game looks like nor what the wiki's table says.
	const float a = dayNightCelestialAngle(tod);
	float c = cosf(a * 6.28318531f);
	if (c < -0.25f) c = -0.25f;
	else if (c > 0.25f) c = 0.25f;

	const float f = 0.5f + 2.0f * c;
	return (1.0f - f) * (float)DAY_SKYDARKEN_MAX;
}

// The published boundaries, transcribed from the Java Edition CLEAR-weather column of
// https://minecraft.wiki/w/Light#Internal_sky_light. Each row is "from this tick onward, the
// sky darkening is this", and the rows run to the end of the day; ticks before the first row
// carry darkening 0.
//
// WHY A TABLE AND NOT THE FORMULA ABOVE. This is the GAMEPLAY answer — the internal sky light
// level a future mob-spawn rule reads — and it has to be bit-identical between a console
// client, a Linux dedicated server and the host suite. `cosf` is not: it is libm's, and libm
// is newlib on one of those and glibc on another. Anywhere the true value sits within an ulp
// of an integer, two libms can truncate it differently, and a spawn rule that disagrees across
// the wire is a desync rather than a wrong pixel. A table of 22 integers cannot do that.
//
// It is also the more ACCURATE answer, and that was measured rather than assumed. Minecraft
// does not use a true cosine either — Mth.cos is a 65,536-entry lookup table — so its
// published boundaries are the LUT's, not a true cosine's. Compared tick by tick across all
// 24,000 ticks of the cycle, the float formula above and this table agree at 23,996 of them.
// The four that differ are t=12541, 12704, 12866 and 23960, and at every one of them the
// float value is within 0.0011 of the integer boundary it straddles:
//
//     t=12541  darkenF = 4.000569093    table says 3
//     t=12704  darkenF = 5.000041440    table says 4
//     t=12866  darkenF = 6.001050001    table says 5
//     t=23960  darkenF = 0.999702896    table says 1
//
// That is exactly the signature of a lookup-table cosine, and it is why the table wins for
// gameplay while the formula stays for rendering, where a 1/15th of a light level for a single
// tick is not visible and a per-platform branch would be worse.
//
// The transcription was checked for completeness, not eyeballed: the twelve published ranges
// were expanded tick by tick and shown to cover all 24,000 ticks of the cycle exactly once,
// with no gap and no overlap. world/daynight_test.c repeats that check against these rows.
typedef struct { uint32_t from; int darken; } DayDarkenRow;

static const DayDarkenRow kDarkenRows[] = {
	{ 12041,  1 }, { 12210,  2 }, { 12377,  3 }, { 12542,  4 },
	{ 12705,  5 }, { 12867,  6 }, { 13027,  7 }, { 13188,  8 },
	{ 13348,  9 }, { 13509, 10 }, { 13670, 11 },
	{ 22331, 10 }, { 22492,  9 }, { 22653,  8 }, { 22813,  7 },
	{ 22974,  6 }, { 23135,  5 }, { 23297,  4 }, { 23460,  3 },
	{ 23624,  2 }, { 23791,  1 }, { 23961,  0 },
};

#define DARKEN_ROWS ((int)(sizeof kDarkenRows / sizeof kDarkenRows[0]))

int dayNightSkyDarken(uint32_t tod)
{
	const uint32_t t = wrap(tod);

	// Walked from the top rather than binary-searched. Twenty-two comparisons at most, once a
	// frame, on a table that fits in a single cache line pair — a binary search here would be
	// more code for no measurable win, and this reads in the same order the table is written.
	int darken = 0;
	for (int i = 0; i < DARKEN_ROWS; i++) {
		if (t < kDarkenRows[i].from) break;
		darken = kDarkenRows[i].darken;
	}
	return darken;
}

int dayNightSkyLight(uint32_t tod)
{
	return DAY_SKYLIGHT_MAX - dayNightSkyDarken(tod);
}

float dayNightLevel(uint32_t tod)
{
	// (15 - darkening) / 15. The shader multiplies the vertex's sky nibble — already divided
	// by 15 — by this, so the product is the internal light level over 15, which is exactly
	// what the wiki's table publishes. At noon this is 1.0 and the frame is byte-identical to
	// the one scene/chunk_render.c draws today with the uniform pinned; at midnight it is
	// 4/15 = 0.2667, a moonlit surface rather than a black one.
	const float darken = dayNightSkyDarkenF(tod);
	return ((float)DAY_SKYLIGHT_MAX - darken) * (1.0f / (float)DAY_SKYLIGHT_MAX);
}

float dayNightSkyTint(uint32_t tod)
{
	// Minecraft's own sky-colour scale: clamp(cos(angle * 2pi) * 2 + 0.5, 0, 1), multiplied
	// into the biome's daytime sky colour. https://minecraft.wiki/w/Sky states the endpoints:
	// "At night, the sky color is always #000000, and during sunset and sunrise this gradually
	// fades with the biome-dependent daytime colors."
	//
	// Note it saturates on the SAME cosine window the light does — cos*2+0.5 leaves [0,1]
	// exactly where cos leaves +-0.25 — so the sky finishes going black on the same tick the
	// ground finishes going dark. That is not a coincidence to be tidied away; it is why the
	// two halves of the cycle look like one event.
	//
	// WHAT IS DELIBERATELY NOT HERE: the orange sunset band. The wiki describes it only
	// qualitatively ("The fog near the setting sun turns into a vibrant orange-red") and
	// publishes no colour for it, and it is a horizon-DIRECTIONAL effect — in Minecraft it is
	// painted around the sun, not across the whole sky. This project's sky is one flat clear
	// colour with no skybox and no gradient, so the only way to imitate it here would be to
	// turn the entire sky orange at dusk, which is not what the game does. Inventing a hex
	// value the source does not publish, to draw an effect in a place the source does not draw
	// it, is exactly the sort of guess this cycle is meant not to contain.
	const float a = dayNightCelestialAngle(tod);
	const float g = cosf(a * 6.28318531f) * 2.0f + 0.5f;
	if (g < 0.0f) return 0.0f;
	if (g > 1.0f) return 1.0f;
	return g;
}

// Scales one 0..255 channel and rounds to nearest. Written once because the clear colour and
// the fog colour must be scaled identically — a half-ulp difference between them is the "grey
// sheet hung in front of the sky" that scene/chunk_render.h's comment warns about.
static uint32_t scaleChannel(uint32_t c, float tint)
{
	const float v = (float)c * tint + 0.5f;
	return (uint32_t)v;
}

uint32_t dayNightSkyClearRgba8(uint32_t tod, uint32_t base_rgba8)
{
	const float tint = dayNightSkyTint(tod);

	// C3D_RenderTargetClear takes 0xRRGGBBAA. The alpha byte is passed through unscaled: it is
	// not a colour, and scaling it would fade the clear itself rather than the sky.
	const uint32_t r = scaleChannel((base_rgba8 >> 24) & 0xFFu, tint);
	const uint32_t g = scaleChannel((base_rgba8 >> 16) & 0xFFu, tint);
	const uint32_t b = scaleChannel((base_rgba8 >>  8) & 0xFFu, tint);
	const uint32_t a = base_rgba8 & 0xFFu;

	return (r << 24) | (g << 16) | (b << 8) | a;
}

uint32_t dayNightSkyFogBgr(uint32_t tod, uint32_t base_rgba8)
{
	const float tint = dayNightSkyTint(tod);

	// The PICA's fog colour register is 0x00BBGGRR, so this is the same three channels in the
	// opposite order. Taking the RGBA8 constant as the input for BOTH functions rather than
	// letting a caller pass SKY_FOG_BGR here is the point: the byte swap happens once, in one
	// place, and it is impossible to scale the fog from one base and the clear from another.
	const uint32_t r = scaleChannel((base_rgba8 >> 24) & 0xFFu, tint);
	const uint32_t g = scaleChannel((base_rgba8 >> 16) & 0xFFu, tint);
	const uint32_t b = scaleChannel((base_rgba8 >>  8) & 0xFFu, tint);

	return (b << 16) | (g << 8) | r;
}

// ── The sidecar ──────────────────────────────────────────────────────────────────────

// 'B','S','T','M'. Byte-wise rather than a packed uint32 so the file is endian-independent by
// construction — world/genversion.c's argument for its own magic, and world/worldseed.c's.
static const uint8_t DTIME_MAGIC[4] = {'B', 'S', 'T', 'M'};

// Builds "<world_dir>/time.bin", answering false and leaving `out` empty when it does not fit.
//
// This is world/worldseed.c's pathFor(), and it is a copy on purpose rather than a shared
// helper: that function is in a file this task does not own, and the measurement recorded
// there — that a truncated path is not a broken path but a perfectly openable name for a
// DIFFERENT file, so writer and reader agree with each other about the wrong name and a
// round-trip reads back perfectly — applies here identically. The consequence is milder for a
// clock than for a seed, but "milder" is not a reason to reintroduce a silent failure.
//
// Using snprintf's RESULT is the fix -Wformat-truncation asks for, and the empty `out` is the
// belt-and-braces second line: it is the one name no fopen can mistake for a neighbour. Both
// callers check the bool and stop before they ever open it.
static bool pathFor(char* out, size_t cap, const char* world_dir)
{
	const int n = snprintf(out, cap, "%s/%s", world_dir, DAY_TIME_FILE);
	if (n < 0 || (size_t)n >= cap) {
		if (cap > 0) out[0] = '\0';
		return false;
	}
	return true;
}

bool dayNightWrite(const char* world_dir, uint64_t ticks)
{
	if (!world_dir || !*world_dir) return false;

	uint8_t buf[DAY_TIME_BYTES];
	memcpy(buf, DTIME_MAGIC, 4);
	for (int i = 0; i < 8; i++)
		buf[4 + i] = (uint8_t)((ticks >> (8 * i)) & 0xFFu);

	const uint32_t crc = crc32(buf, 12);
	buf[12] = (uint8_t)(crc & 0xFFu);
	buf[13] = (uint8_t)((crc >> 8) & 0xFFu);
	buf[14] = (uint8_t)((crc >> 16) & 0xFFu);
	buf[15] = (uint8_t)((crc >> 24) & 0xFFu);

	char path[160];
	if (!pathFor(path, sizeof path, world_dir)) return false;

	FILE* f = fopen(path, "wb");
	if (!f) return false;
	const bool ok = fwrite(buf, 1, sizeof buf, f) == sizeof buf;
	// fclose can fail on a full card with the bytes still in the buffer, so its result is part
	// of the answer and not ignored — world/worldseed.c's line, for the same reason.
	return (fclose(f) == 0) && ok;
}

DayTimeStatus dayNightRead(const char* world_dir, uint64_t* out)
{
	uint64_t dummy;
	if (!out) out = &dummy;

	// Written FIRST and on every path. The header promises a caller that ignores the status
	// still gets a usable world, and the only way to keep that promise is for the default to
	// be in place before any branch can return.
	*out = DAY_START_TICKS;

	if (!world_dir || !*world_dir) return DAYTIME_NO_WORLD_DIR;

	// A path that would not fit cannot be reported as "no sidecar": that is a fault answered
	// as a fact about the world's history. It is also not safe to fall through to the fopen —
	// a truncated path that DOES open reports some other file's contents as this world's
	// clock. world/worldseed.c found exactly that bug in its own reader and this is its fix.
	char path[160];
	if (!pathFor(path, sizeof path, world_dir)) return DAYTIME_DAMAGED;

	FILE* f = fopen(path, "rb");
	if (!f) return DAYTIME_ABSENT;

	uint8_t buf[DAY_TIME_BYTES];
	const size_t got = fread(buf, 1, sizeof buf, f);
	// A file LONGER than the record is as wrong as a short one — nothing this build writes
	// produces one, so something else wrote here and the contents cannot be trusted.
	const bool trailing = (fgetc(f) != EOF);
	fclose(f);

	if (got != sizeof buf || trailing)          return DAYTIME_DAMAGED;
	if (memcmp(buf, DTIME_MAGIC, 4) != 0)       return DAYTIME_DAMAGED;

	const uint32_t want = (uint32_t)buf[12] | ((uint32_t)buf[13] << 8) |
	                      ((uint32_t)buf[14] << 16) | ((uint32_t)buf[15] << 24);
	if (crc32(buf, 12) != want)                 return DAYTIME_DAMAGED;

	uint64_t ticks = 0;
	for (int i = 0; i < 8; i++)
		ticks |= (uint64_t)buf[4 + i] << (8 * i);

	// Every 64-bit value is a legal counter — it is a count of ticks, it carries no capability
	// and there is no version of this file that could not read one — so there is no equivalent
	// of world/genversion.h's GENVER_TOO_NEW here and there must not be one.
	*out = ticks;
	return DAYTIME_OK;
}
