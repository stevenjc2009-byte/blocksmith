// Host tests for the day/night clock (world/daynight.c, v1.8.9).
//
// ── The oracle, and why this file is worth more than its assertions ──────────────────
//
// Almost every check below is a HAND-WRITTEN LITERAL taken off minecraft.wiki, not a value
// derived from anything world/daynight.c computes. That distinction is this project's recorded
// case-3 trap — a check parameterised by the value under test cannot detect that value
// changing — and it matters more here than usual, because daynight.c contains a transcribed
// table (kDarkenRows) and a test that read that table back would be checking a copy against
// itself.
//
// The oracle is the CLEAR-weather, Java Edition column of the "Internal sky light" table at
//
//     https://minecraft.wiki/w/Light#Internal_sky_light
//
// reproduced below as kWikiSkyLight[] from the published wikitext. Twelve ranges, level 15 down
// to level 4. daynight.c's own table is a different transcription of the same source in a
// different shape (boundary rows rather than ranges), so the two agreeing is a real check on
// both, and a typo in either is caught.
//
// ── What is checked, in order of what would hurt most if it were wrong ───────────────
//
//   1. THE CURVE IS NOT A SINE. The single most likely wrong implementation of a day/night
//      cycle is a smooth fade from noon to midnight, and it is wrong in a way that looks
//      plausible in a screenshot: it darkens the world at 09:00. testCurveIsNotASine pins the
//      brightness as EXACTLY full across the whole of the day and EXACTLY minimum across the
//      whole of the night, and pins the widths of the two ramps between them. A sine fails
//      every one of those and a clamped cosine passes all of them.
//   2. EVERY PUBLISHED BOUNDARY, both sides. Each of the twelve wiki ranges is checked at its
//      first tick, at its last tick, and at the tick either side of it — so an off-by-one in
//      the table cannot hide inside a range that is otherwise right.
//   3. THE PARTITION. The twelve ranges are expanded tick by tick and shown to cover all
//      24,000 ticks exactly once, with no gap and no overlap, and dayNightSkyLight is compared
//      against that expansion at every one of them. A table with a missing row passes a
//      spot-check and fails this.
//   4. THE FLOAT/TABLE DISAGREEMENT, AS A PINNED NUMBER. daynight.c claims its rendering
//      formula and its gameplay table differ at exactly four ticks, named. That claim is a
//      measurement, so it is checked as one: not "they mostly agree" but "they disagree at
//      12541, 12704, 12866 and 23960, and nowhere else".
//   5. SAVE AND RELOAD. A world saved at dusk comes back at dusk, an OLD world with no
//      time.bin comes back in the morning rather than refusing, and every way of damaging the
//      sidecar is a harmless morning rather than a wrong evening.
//
// ── The __3DS__ guard is load-bearing ────────────────────────────────────────────────
//
// The console Makefile globs every .c under source/world, so without it this file's main()
// would collide with source/main.c's. Same as world/tick_test.c.
#ifndef __3DS__

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "world/daynight.h"

static int g_checks = 0;
static int g_fails  = 0;

static void check(bool cond, const char *what)
{
	g_checks++;
	if (!cond) {
		g_fails++;
		printf("FAIL  %s\n", what);
	}
}

static void checkf(bool cond, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void checkf(bool cond, const char *fmt, ...)
{
	g_checks++;
	if (!cond) {
		g_fails++;
		va_list ap;
		va_start(ap, fmt);
		printf("FAIL  ");
		vprintf(fmt, ap);
		printf("\n");
		va_end(ap);
	}
}

// ── The oracle ───────────────────────────────────────────────────────────────────────
//
// https://minecraft.wiki/w/Light#Internal_sky_light, Java Edition, clear weather. Transcribed
// from the raw wikitext. Ranges are inclusive. Level 15's published range 23,961-12,040 wraps
// midnight and is written here as the two halves it is; level 4's 13,670-22,330 does not wrap.
// Levels 14 down to 5 each publish two ranges, one either side of the night.
typedef struct { int level; uint32_t a, b; } WikiRange;

static const WikiRange kWikiSkyLight[] = {
	{ 15, 23961, 23999 }, { 15,     0, 12040 },
	{ 14, 23791, 23960 }, { 14, 12041, 12209 },
	{ 13, 23624, 23790 }, { 13, 12210, 12376 },
	{ 12, 23460, 23623 }, { 12, 12377, 12541 },
	{ 11, 23297, 23459 }, { 11, 12542, 12704 },
	{ 10, 23135, 23296 }, { 10, 12705, 12866 },
	{  9, 22974, 23134 }, {  9, 12867, 13026 },
	{  8, 22813, 22973 }, {  8, 13027, 13187 },
	{  7, 22653, 22812 }, {  7, 13188, 13347 },
	{  6, 22492, 22652 }, {  6, 13348, 13508 },
	{  5, 22331, 22491 }, {  5, 13509, 13669 },
	{  4, 13670, 22330 },
};

#define WIKI_ROWS ((int)(sizeof kWikiSkyLight / sizeof kWikiSkyLight[0]))

// ── 1. The constants ─────────────────────────────────────────────────────────────────

static void testConstantsPinned(void)
{
	// Naked literals, every one. Not `== DAY_TICKS` restated, and not arithmetic on another
	// constant in the same header — those pass whatever the header says.
	check(DAY_TICKS == 24000u,
	      "DAY_TICKS is 24000 - a Minecraft day is 24,000 ticks / 20 real minutes");
	check(DAY_TICK_DAWN == 0u,        "dawn is tick 0 (06:00)");
	check(DAY_TICK_DAY == 1000u,      "/time set day is tick 1000");
	check(DAY_TICK_NOON == 6000u,     "/time set noon is tick 6000 (12:00)");
	check(DAY_TICK_SUNSET == 12000u,  "sunset begins at tick 12000 (18:00)");
	check(DAY_TICK_NIGHT == 13000u,   "night begins at tick 13000 (19:00)");
	check(DAY_TICK_MIDNIGHT == 18000u,"midnight is tick 18000 (00:00)");
	check(DAY_TICK_SUNRISE == 23000u, "sunrise begins at tick 23000 (05:00)");
	check(DAY_START_TICKS == 1000u,   "a new world starts at tick 1000, morning");

	check(DAY_SKYLIGHT_MAX == 15,     "noon internal sky light is 15");
	check(DAY_SKYLIGHT_MIN == 4,      "midnight internal sky light is 4, not 0");
	check(DAY_SKYDARKEN_MAX == 11,    "getSkyDarken maxes at 11");

	// The published phase LENGTHS, checked as durations rather than as boundaries, because a
	// pair of boundaries can both be wrong in the same direction and still subtract correctly
	// only if the length is checked too.
	check(DAY_TICK_SUNSET - DAY_TICK_DAWN == 12000u,
	      "daytime is 12,000 ticks - the wiki's 10 minutes");
	check(DAY_TICK_NIGHT - DAY_TICK_SUNSET == 1000u,
	      "sunset is 1,000 ticks - the wiki's 50 seconds");
	check(DAY_TICK_SUNRISE - DAY_TICK_NIGHT == 10000u,
	      "night is 10,000 ticks - the wiki's 8 minutes 20 seconds");
	check(DAY_TICKS - DAY_TICK_SUNRISE == 1000u,
	      "sunrise is 1,000 ticks - the wiki's 50 seconds");
}

// ── 2. Phases ────────────────────────────────────────────────────────────────────────

static void testPhaseBoundaries(void)
{
	// Every boundary from both sides. A `<=` written where a `<` belongs moves exactly one of
	// these and nothing else in the file.
	check(dayNightPhase(0)     == DAY_PHASE_DAY,     "tick 0 is day");
	check(dayNightPhase(11999) == DAY_PHASE_DAY,     "tick 11999 is the last day tick");
	check(dayNightPhase(12000) == DAY_PHASE_SUNSET,  "tick 12000 is the first sunset tick");
	check(dayNightPhase(12999) == DAY_PHASE_SUNSET,  "tick 12999 is the last sunset tick");
	check(dayNightPhase(13000) == DAY_PHASE_NIGHT,   "tick 13000 is the first night tick");
	check(dayNightPhase(22999) == DAY_PHASE_NIGHT,   "tick 22999 is the last night tick");
	check(dayNightPhase(23000) == DAY_PHASE_SUNRISE, "tick 23000 is the first sunrise tick");
	check(dayNightPhase(23999) == DAY_PHASE_SUNRISE, "tick 23999 is the last tick of the day");

	// Out-of-range inputs reduce rather than fault - the header promises it, and a caller
	// handing over a whole counter instead of a time of day is the obvious way to get one.
	check(dayNightPhase(24000)  == DAY_PHASE_DAY,   "tick 24000 wraps to day");
	check(dayNightPhase(48000)  == DAY_PHASE_DAY,   "tick 48000 wraps to day");
	check(dayNightPhase(37000)  == DAY_PHASE_NIGHT, "tick 37000 wraps to 13000, night");
}

// ── 3. The wiki table, boundary by boundary ─────────────────────────────────────────

static void testWikiBoundariesBothSides(void)
{
	for (int i = 0; i < WIKI_ROWS; i++) {
		const WikiRange r = kWikiSkyLight[i];

		checkf(dayNightSkyLight(r.a) == r.level,
		       "wiki: sky light %d starts at tick %u (got %d)",
		       r.level, r.a, dayNightSkyLight(r.a));
		checkf(dayNightSkyLight(r.b) == r.level,
		       "wiki: sky light %d ends at tick %u (got %d)",
		       r.level, r.b, dayNightSkyLight(r.b));

		// One tick OUTSIDE each end must NOT be this level. This is the half that catches an
		// off-by-one: a table whose row starts one tick early still passes both checks above.
		// Skipped where the neighbour is inside another range of the same level (level 15's
		// two halves meet across the 0/23999 wrap).
		const uint32_t before = (r.a + DAY_TICKS - 1u) % DAY_TICKS;
		const uint32_t after  = (r.b + 1u) % DAY_TICKS;
		const bool wrap_join  = (r.a == 0u) || (r.b == 23999u);
		if (!wrap_join) {
			checkf(dayNightSkyLight(before) != r.level,
			       "wiki: tick %u, one before the sky light %d range, is not %d",
			       before, r.level, r.level);
			checkf(dayNightSkyLight(after) != r.level,
			       "wiki: tick %u, one after the sky light %d range, is not %d",
			       after, r.level, r.level);
		}
	}
}

// ── 4. The partition ─────────────────────────────────────────────────────────────────

static void testWikiTableIsAPartitionAndMatches(void)
{
	// Expanded from the oracle, not from daynight.c.
	static int want[24000];
	for (int t = 0; t < 24000; t++) want[t] = -1;

	int overlaps = 0;
	for (int i = 0; i < WIKI_ROWS; i++) {
		const WikiRange r = kWikiSkyLight[i];
		for (uint32_t t = r.a; t <= r.b; t++) {
			if (want[t] != -1) overlaps++;
			want[t] = r.level;
		}
	}

	int gaps = 0;
	for (int t = 0; t < 24000; t++) if (want[t] == -1) gaps++;

	check(overlaps == 0, "the twelve published ranges do not overlap");
	check(gaps == 0,     "the twelve published ranges leave no tick uncovered");

	int wrong = 0, first_wrong = -1;
	for (int t = 0; t < 24000; t++) {
		if (dayNightSkyLight((uint32_t)t) != want[t]) {
			wrong++;
			if (first_wrong < 0) first_wrong = t;
		}
	}
	checkf(wrong == 0,
	       "dayNightSkyLight matches the wiki at all 24,000 ticks (%d wrong, first at t=%d: "
	       "got %d want %d)",
	       wrong, first_wrong,
	       first_wrong >= 0 ? dayNightSkyLight((uint32_t)first_wrong) : 0,
	       first_wrong >= 0 ? want[first_wrong] : 0);

	// dayNightSkyDarken is the same statement from the other side. Checked separately so a
	// wrong DAY_SKYLIGHT_MAX cannot cancel out against a wrong table.
	int darken_wrong = 0;
	for (int t = 0; t < 24000; t++)
		if (dayNightSkyDarken((uint32_t)t) != 15 - want[t]) darken_wrong++;
	checkf(darken_wrong == 0,
	       "dayNightSkyDarken is 15 minus the wiki's level at all 24,000 ticks (%d wrong)",
	       darken_wrong);

	// Range, as its own statement. A table that returned 0 everywhere would satisfy a
	// carelessly written comparison but not this.
	int lo = 99, hi = -99;
	for (int t = 0; t < 24000; t++) {
		const int v = dayNightSkyLight((uint32_t)t);
		if (v < lo) lo = v;
		if (v > hi) hi = v;
	}
	checkf(lo == 4, "the darkest the sky ever gets is internal light 4 (got %d)", lo);
	checkf(hi == 15, "the brightest the sky ever gets is internal light 15 (got %d)", hi);
}

// ── 5. The curve is not a sine ───────────────────────────────────────────────────────

static void testCurveIsNotASine(void)
{
	// THE CHECK THIS WHOLE FILE EXISTS FOR.
	//
	// A naive `0.5 + 0.5*cos(2*pi*t/24000)` fade — the obvious wrong implementation — is at
	// roughly 0.93, 0.75 and 0.50 of full at ticks 3000, 4500 and 9000. Minecraft is at
	// EXACTLY full at all three, because its cosine is clamped before it is used. These are
	// exact-equality checks on 1.0f deliberately: the real curve is not "close to" full
	// through the day, it is saturated, and an approximate comparison would accept the fade
	// this check exists to reject.
	const uint32_t day_ticks[]   = { 200, 1000, 3000, 4500, 6000, 7500, 9000, 11000, 11867 };
	for (unsigned i = 0; i < sizeof day_ticks / sizeof day_ticks[0]; i++)
		checkf(dayNightLevel(day_ticks[i]) == 1.0f,
		       "dayLevel is EXACTLY 1.0 in full day at tick %u (got %.6f) - a sine would not "
		       "be", day_ticks[i], (double)dayNightLevel(day_ticks[i]));

	// The same statement at the other end. 4/15 exactly, not "dark".
	const float night = 4.0f / 15.0f;
	const uint32_t night_ticks[] = { 13670, 14000, 16000, 18000, 20000, 22000, 22330 };
	for (unsigned i = 0; i < sizeof night_ticks / sizeof night_ticks[0]; i++)
		checkf(fabsf(dayNightLevel(night_ticks[i]) - night) < 1e-6f,
		       "dayLevel is EXACTLY 4/15 in full night at tick %u (got %.6f)",
		       night_ticks[i], (double)dayNightLevel(night_ticks[i]));

	// The transitions, as WIDTHS. This is what makes the two blocks above meaningful: a curve
	// that is flat at both ends but takes six hours to get between them is still not
	// Minecraft. Measured on this implementation and pinned as literals.
	int flat_day = 0, flat_night = 0, ramp = 0;
	for (int t = 0; t < 24000; t++) {
		const float v = dayNightLevel((uint32_t)t);
		if (v == 1.0f) flat_day++;
		else if (fabsf(v - night) < 1e-6f) flat_night++;
		else ramp++;
	}
	checkf(flat_day == 11735,
	       "the sky is at FULL brightness for 11,735 of 24,000 ticks (got %d)", flat_day);
	checkf(flat_night == 8661,
	       "the sky is at FULL dark for 8,661 of 24,000 ticks (got %d)", flat_night);
	checkf(ramp == 3604,
	       "only 3,604 of 24,000 ticks are in transition at all (got %d)", ramp);
	checkf(flat_day + flat_night + ramp == 24000,
	       "the three windows account for the whole cycle (got %d)",
	       flat_day + flat_night + ramp);

	// And the single number that separates the two implementations most cleanly: the fraction
	// of the cycle spent NOT at one of the two settings. A pure sine spends 24,000 ticks in
	// transition; this spends 3,604, which is 15%.
	check(ramp * 100 / 24000 == 15,
	      "15% of the cycle is transition - a sine would be 100%");

	// The night floor is a floor and not zero. A cycle that took the world to black would be
	// darker than the game it copies, and it is an easy mistake to make by scaling to 0..1
	// instead of to 4/15..1.
	float lowest = 2.0f;
	for (int t = 0; t < 24000; t++) {
		const float v = dayNightLevel((uint32_t)t);
		if (v < lowest) lowest = v;
	}
	checkf(lowest > 0.26f && lowest < 0.27f,
	       "the darkest dayLevel is 4/15 = 0.2667, never 0 (got %.6f)", (double)lowest);

	// THE UNIFORM'S CONTRACT, as a bound over the whole cycle. The shader does
	// `mul r5.z, dayLevel.xxxx, r5.xxxx` and then maxes against block light — it does NOT
	// clamp — so a value above 1.0 makes a face brighter than its own texel and a negative one
	// inverts it. Neither would look like a bug in the clock; both would look like broken art.
	// The naive-sine arm produced 2.100000 at noon and -0.833333 at midnight, which is exactly
	// why this is checked as a range and not only at sampled ticks.
	int out_of_range = 0;
	for (int t = 0; t < 24000; t++) {
		const float v = dayNightLevel((uint32_t)t);
		if (!(v >= night - 1e-6f && v <= 1.0f)) out_of_range++;
	}
	checkf(out_of_range == 0,
	       "dayLevel stays inside [4/15, 1.0] at all 24,000 ticks - the shader does not clamp "
	       "it (%d ticks outside)", out_of_range);

	// Continuity: no single tick may move dayLevel by more than a small step, or the
	// transition would strobe rather than fade.
	float worst_step = 0.0f;
	for (int t = 0; t < 24000; t++) {
		const float a = dayNightLevel((uint32_t)t);
		const float b = dayNightLevel((uint32_t)((t + 1) % 24000));
		const float d = fabsf(b - a);
		if (d > worst_step) worst_step = d;
	}
	checkf(worst_step < 0.001f,
	       "no single tick moves dayLevel by as much as 0.001 (worst %.6f)",
	       (double)worst_step);
}

// ── 6. The float formula against the table ───────────────────────────────────────────

static void testFloatFormulaAgainstTable(void)
{
	// daynight.c states as a MEASUREMENT that its rendering formula and its gameplay table
	// disagree at exactly four ticks, and names them. A measurement in a comment that nothing
	// checks is a claim, so this checks it.
	static const uint32_t kKnownDisagreements[] = { 12541, 12704, 12866, 23960 };
	const int n_known = (int)(sizeof kKnownDisagreements / sizeof kKnownDisagreements[0]);

	int disagreements = 0;
	uint32_t found[16];
	for (int t = 0; t < 24000; t++) {
		const int from_float = (int)dayNightSkyDarkenF((uint32_t)t);
		const int from_table = dayNightSkyDarken((uint32_t)t);
		if (from_float != from_table) {
			if (disagreements < 16) found[disagreements] = (uint32_t)t;
			disagreements++;
		}
	}

	checkf(disagreements == n_known,
	       "the float formula and the pinned table disagree at exactly %d of 24,000 ticks "
	       "(got %d)", n_known, disagreements);

	if (disagreements == n_known) {
		for (int i = 0; i < n_known; i++)
			checkf(found[i] == kKnownDisagreements[i],
			       "disagreement %d is at tick %u (got %u)",
			       i, kKnownDisagreements[i], found[i]);
	} else {
		// Keep the check count stable whichever way the arm goes, so a red run cannot also
		// look like checks going missing.
		for (int i = 0; i < n_known; i++)
			check(false, "disagreement list not comparable - the count is already wrong");
	}

	// Every disagreement must be a rounding straddle and not a real divergence: the float
	// value has to be within a thousandth of the integer boundary it lands the wrong side of.
	for (int i = 0; i < n_known; i++) {
		const float v = dayNightSkyDarkenF(kKnownDisagreements[i]);
		const float nearest = floorf(v + 0.5f);
		checkf(fabsf(v - nearest) < 0.0011f,
		       "tick %u disagrees only because %.9f straddles %.1f",
		       kKnownDisagreements[i], (double)v, (double)nearest);
	}
}

// ── 7. The celestial angle ───────────────────────────────────────────────────────────

static void testCelestialAngle(void)
{
	// The wiki's Sky angle formula has 0 degrees at NOON, so the angle must be 0 at tick 6000
	// and half a rotation at tick 18000. Getting this backwards puts the sun underground at
	// midday and is invisible to every brightness check in this file, because the brightness
	// curve is symmetric about it.
	checkf(fabsf(dayNightCelestialAngle(6000) - 0.0f) < 1e-6f,
	       "the celestial angle is 0 rotations at noon (got %.6f)",
	       (double)dayNightCelestialAngle(6000));
	checkf(fabsf(dayNightCelestialAngle(18000) - 0.5f) < 1e-6f,
	       "the celestial angle is 0.5 rotations at midnight (got %.6f)",
	       (double)dayNightCelestialAngle(18000));

	// The angle must rise monotonically through the day and wrap exactly ONCE, and the tick it
	// wraps on is a real check rather than bookkeeping: the wrap is where the angle is zero,
	// and zero is noon. An implementation that made 0 rotations mean DAWN — the natural
	// mistake, because tick 0 is dawn — would wrap here at 23999 instead, and would still pass
	// every brightness check in this file, because the brightness curve is symmetric about
	// noon and cannot see the difference.
	int wraps = 0, last_wrap = -1;
	for (int t = 0; t < 23999; t++) {
		if (dayNightCelestialAngle((uint32_t)(t + 1)) <= dayNightCelestialAngle((uint32_t)t)) {
			wraps++;
			last_wrap = t;
		}
	}
	checkf(wraps == 1,
	       "the sky angle falls exactly once within a day - it is otherwise monotonic (%d)",
	       wraps);
	checkf(last_wrap == 5999,
	       "and the one fall is the wrap from tick 5999 to tick 6000, which is what says 0 "
	       "rotations is NOON and not dawn (got %d)", last_wrap);
	check(dayNightCelestialAngle(5999) > 0.9999f,
	      "tick 5999, one before noon, is very nearly a whole rotation");

	// THE NON-LINEARITY, and a CONTRADICTION IN THE SOURCE that is pinned rather than papered
	// over. https://minecraft.wiki/w/Daylight_cycle's Sky angle prose claims the sky "move[s]
	// faster during noon and midnight and slower during sunrises and sunsets". The formula
	// printed directly beneath that sentence does not do that, and neither does the game's
	// timeOfDay function it reduces to: the rate is 2/3 + pi*sin(pi*d)/6, minimised at noon and
	// maximised at midnight. This file follows the FORMULA.
	//
	// A linear ramp - the obvious wrong implementation - would make all three rates equal and
	// every ratio below exactly 1.
	const float r_noon = dayNightCelestialAngle(6100)  - dayNightCelestialAngle(6000);
	const float r_dusk = dayNightCelestialAngle(12100) - dayNightCelestialAngle(12000);
	const float r_midn = dayNightCelestialAngle(18100) - dayNightCelestialAngle(18000);

	checkf(r_noon < r_dusk && r_dusk < r_midn,
	       "the sky rotates slowest at noon, faster at dusk, fastest at midnight "
	       "(noon %.7f, dusk %.7f, midnight %.7f) - the wiki's PROSE says the opposite about "
	       "noon; its own formula and the game's source say this",
	       (double)r_noon, (double)r_dusk, (double)r_midn);

	// The measured ratios, pinned. Bands rather than equalities because these are floats, but
	// narrow enough that a different curve cannot sit inside them.
	const float ratio_midnight = r_midn / r_noon;
	const float ratio_dusk     = r_dusk / r_noon;
	checkf(ratio_midnight > 1.77f && ratio_midnight < 1.79f,
	       "midnight rotates 1.776x as fast as noon (got %.6f) - a linear ramp is 1.0",
	       (double)ratio_midnight);
	checkf(ratio_dusk > 1.54f && ratio_dusk < 1.57f,
	       "dusk rotates 1.551x as fast as noon (got %.6f)", (double)ratio_dusk);
}

// ── 8. Sky colour ────────────────────────────────────────────────────────────────────

static void testSkyColour(void)
{
	// scene/chunk_render.h's constant, written here as a literal so this file does not need
	// that header (it pulls in <3ds.h> a long way down) and so the value is pinned rather than
	// followed.
	const uint32_t base_rgba8 = 0x102A33FFu;

	check(dayNightSkyClearRgba8(6000, base_rgba8) == 0x102A33FFu,
	      "at noon the sky clear colour is exactly the unscaled base");
	check(dayNightSkyFogBgr(6000, base_rgba8) == 0x00332A10u,
	      "at noon the fog colour is exactly SKY_FOG_BGR - the byte swap is right");

	// https://minecraft.wiki/w/Sky: "At night, the sky color is always #000000".
	check(dayNightSkyClearRgba8(18000, base_rgba8) == 0x000000FFu,
	      "at midnight the sky is black, with the alpha byte passed through unscaled");
	check(dayNightSkyFogBgr(18000, base_rgba8) == 0x00000000u,
	      "at midnight the fog colour is black too, so the world does not fade to a lit "
	      "horizon under a black sky");

	// The alpha byte must never be scaled. It is not a colour, and fading it fades the clear
	// itself.
	int alpha_wrong = 0;
	for (int t = 0; t < 24000; t += 7)
		if ((dayNightSkyClearRgba8((uint32_t)t, base_rgba8) & 0xFFu) != 0xFFu) alpha_wrong++;
	checkf(alpha_wrong == 0,
	       "the clear colour's alpha byte is 0xFF at every sampled tick (%d were not)",
	       alpha_wrong);

	// The two byte orders must always describe the SAME colour. This is the drift
	// chunk_render.h warns about, and it is checked at every tick rather than at two.
	int mismatched = 0;
	for (int t = 0; t < 24000; t++) {
		const uint32_t c = dayNightSkyClearRgba8((uint32_t)t, base_rgba8);
		const uint32_t f = dayNightSkyFogBgr((uint32_t)t, base_rgba8);
		const uint32_t cr = (c >> 24) & 0xFFu, cg = (c >> 16) & 0xFFu, cb = (c >> 8) & 0xFFu;
		const uint32_t fr = f & 0xFFu, fg = (f >> 8) & 0xFFu, fb = (f >> 16) & 0xFFu;
		if (cr != fr || cg != fg || cb != fb) mismatched++;
	}
	checkf(mismatched == 0,
	       "the clear colour and the fog colour are the same colour at all 24,000 ticks "
	       "(%d disagreed)", mismatched);

	// The tint saturates on the same window the light does - both clamp the same cosine at
	// the same +-0.25, so the sky finishes going black on the tick the ground finishes going
	// dark. Measured and pinned.
	check(dayNightSkyTint(13670) == 0.0f,
	      "the sky reaches full black at tick 13670, the same tick the sky light reaches 4");
	check(dayNightSkyTint(13669) > 0.0f,
	      "and not one tick earlier");
	check(dayNightSkyTint(22330) == 0.0f,
	      "the sky stays black through tick 22330, the last tick of sky light 4");
	check(dayNightSkyTint(22331) > 0.0f,
	      "and starts coming back on 22331, the tick sky light leaves 4");
}

// ── 9. The counter ───────────────────────────────────────────────────────────────────

static void testCounterAndWrap(void)
{
	DayNight d;
	dayNightInit(&d, 0);
	check(dayNightTicks(&d) == 0u,       "a clock initialised at 0 is at 0");
	check(dayNightTimeOfDay(&d) == 0u,   "and its time of day is 0");
	check(dayNightDay(&d) == 0u,         "and it is day 0");
	check(dayNightMoonPhase(&d) == 0u,   "and moon phase 0");

	// One whole day, one tick at a time. This is the wrap, and it is walked rather than
	// jumped: an implementation that wraps with a subtraction rather than a modulo is correct
	// for one day and wrong for the second.
	dayNightInit(&d, 0);
	for (int i = 0; i < 24000; i++) dayNightAdvance(&d, 1);
	check(dayNightTicks(&d) == 24000u,     "24,000 single-tick advances land on 24,000");
	check(dayNightTimeOfDay(&d) == 0u,     "which is time of day 0 again");
	check(dayNightDay(&d) == 1u,           "and day 1");

	// Three more days in ragged steps, the way tickClockAdvance actually returns them.
	const int steps[] = { 4, 1, 3, 2, 4, 1, 1, 4, 2, 3 };   // sums to 25
	int fed = 0;
	while (fed < 72000) {
		for (unsigned i = 0; i < sizeof steps / sizeof steps[0] && fed < 72000; i++) {
			int n = steps[i];
			if (fed + n > 72000) n = 72000 - fed;
			dayNightAdvance(&d, n);
			fed += n;
		}
	}
	check(dayNightTicks(&d) == 96000u,   "three more days in ragged steps reach 96,000");
	check(dayNightDay(&d) == 4u,         "which is day 4");
	check(dayNightTimeOfDay(&d) == 0u,   "at time of day 0");

	// The moon phase is an 8-day cycle over the WHOLE counter, so it needs a counter that has
	// not been wrapped away.
	dayNightInit(&d, 24000u * 7u + 500u);
	check(dayNightMoonPhase(&d) == 7u,  "day 7 is moon phase 7");
	dayNightInit(&d, 24000u * 8u + 500u);
	check(dayNightMoonPhase(&d) == 0u,  "day 8 wraps the moon back to phase 0");
	dayNightInit(&d, 24000u * 13u + 500u);
	check(dayNightMoonPhase(&d) == 5u,  "day 13 is moon phase 5");

	// A negative advance rewinds nothing.
	dayNightInit(&d, 5000);
	dayNightAdvance(&d, -100);
	check(dayNightTicks(&d) == 5000u, "a negative advance does not rewind the day");
	dayNightAdvance(&d, 0);
	check(dayNightTicks(&d) == 5000u, "a zero advance does not move it either");

	// dayNightSet is the only non-monotonic route, and it is the one a server correction and
	// a debug jump both use.
	dayNightSet(&d, 18000);
	check(dayNightTicks(&d) == 18000u,     "dayNightSet moves the clock outright");
	check(dayNightTimeOfDay(&d) == 18000u, "to midnight");

	// Far from the origin. A uint32 counter would have wrapped long before this.
	dayNightInit(&d, 100000000000ull + 12345ull);
	check(dayNightTicks(&d) == 100000012345ull,
	      "the counter holds a hundred billion ticks without truncating");
	check(dayNightTimeOfDay(&d) == 100000012345ull % 24000ull,
	      "and its time of day is still right there");

	// NULL safety on every entry point, the way world/tick.c is checked.
	dayNightInit(NULL, 5);
	dayNightAdvance(NULL, 5);
	dayNightSet(NULL, 5);
	check(dayNightTicks(NULL) == 0u,                 "dayNightTicks(NULL) is 0");
	check(dayNightTimeOfDay(NULL) == DAY_START_TICKS,"dayNightTimeOfDay(NULL) is the default");
	check(dayNightDay(NULL) == 0u,                   "dayNightDay(NULL) is 0");
	check(dayNightMoonPhase(NULL) == 0u,             "dayNightMoonPhase(NULL) is 0");
}

static void testDeterminism(void)
{
	// The same total, reached by two different routes, must give the same clock. This is what
	// makes a saved counter meaningful: a world that resumes has taken a different route to
	// the same number than one that ran continuously.
	DayNight a, b;
	dayNightInit(&a, 1000);
	for (int i = 0; i < 30000; i++) dayNightAdvance(&a, 1);

	dayNightInit(&b, 1000);
	for (int i = 0; i < 7500; i++) dayNightAdvance(&b, 4);

	check(dayNightTicks(&a) == dayNightTicks(&b),
	      "30,000 single ticks and 7,500 quadruple ticks reach the same counter");
	check(dayNightTimeOfDay(&a) == dayNightTimeOfDay(&b),
	      "and therefore the same time of day");
	check(dayNightLevel(dayNightTimeOfDay(&a)) == dayNightLevel(dayNightTimeOfDay(&b)),
	      "and the same dayLevel, bit for bit");

	// Every pure function must be a function of the time of day ALONE - the same tick in day 0
	// and in day 900 must look identical, or the world would slowly drift brighter or darker
	// over a long-lived save.
	int drifted = 0;
	for (int t = 0; t < 24000; t += 13) {
		DayNight early, late;
		dayNightInit(&early, (uint64_t)t);
		dayNightInit(&late,  24000ull * 900ull + (uint64_t)t);
		if (dayNightLevel(dayNightTimeOfDay(&early)) !=
		    dayNightLevel(dayNightTimeOfDay(&late))) drifted++;
		if (dayNightSkyLight(dayNightTimeOfDay(&early)) !=
		    dayNightSkyLight(dayNightTimeOfDay(&late))) drifted++;
	}
	checkf(drifted == 0,
	       "tick t of day 0 and tick t of day 900 light the world identically (%d drifted)",
	       drifted);
}

// ── 10. The sidecar ──────────────────────────────────────────────────────────────────

static char g_dir[256];

static void writeRaw(const char *name, const void *data, size_t len)
{
	char path[512];
	snprintf(path, sizeof path, "%s/%s", g_dir, name);
	FILE *f = fopen(path, "wb");
	if (f) { fwrite(data, 1, len, f); fclose(f); }
}

static void removeSidecar(void)
{
	char path[512];
	snprintf(path, sizeof path, "%s/%s", g_dir, DAY_TIME_FILE);
	remove(path);
}

static void testSidecarRoundTrip(void)
{
	check(DAY_TIME_BYTES == 16, "the sidecar is 16 bytes: 4 magic + 8 ticks + 4 CRC");
	check(strcmp(DAY_TIME_FILE, "time.bin") == 0, "and it is called time.bin");

	// A world saved at dusk comes back at dusk. THE REQUIREMENT, checked with the actual
	// dusk value rather than with a round number.
	removeSidecar();
	const uint64_t dusk = 24000ull * 5ull + 12600ull;
	check(dayNightWrite(g_dir, dusk), "the sidecar writes");

	uint64_t got = 0;
	check(dayNightRead(g_dir, &got) == DAYTIME_OK, "and reads back OK");
	check(got == dusk, "with the exact counter it was given");

	DayNight d;
	dayNightInit(&d, got);
	check(dayNightTimeOfDay(&d) == 12600u, "a world saved at dusk reopens at dusk");
	check(dayNightPhase(dayNightTimeOfDay(&d)) == DAY_PHASE_SUNSET,
	      "and the phase it reopens in is sunset, not day");
	check(dayNightDay(&d) == 5u, "and on day 5, not day 0");

	// A big counter survives the eight bytes. A four-byte field would fail here and nowhere
	// else in this file.
	const uint64_t big = 0x0123456789ABCDEFull;
	check(dayNightWrite(g_dir, big), "a 64-bit counter writes");
	check(dayNightRead(g_dir, &got) == DAYTIME_OK && got == big,
	      "and survives the round trip with all eight bytes intact");

	// Zero is a legal counter and must not read as absent.
	check(dayNightWrite(g_dir, 0), "a counter of 0 writes");
	check(dayNightRead(g_dir, &got) == DAYTIME_OK && got == 0u,
	      "and reads back as OK with 0, not as an absent sidecar");
}

static void testOldSaveStillLoads(void)
{
	// THE MIGRATION CHECK. A world made before v1.8.9 has region files and a seed sidecar and
	// no time.bin at all. It must open, in the morning, without a format version anywhere
	// being bumped.
	removeSidecar();

	// Populate the directory the way a real old world is populated, so "absent" is a statement
	// about time.bin and not about an empty directory.
	const uint8_t fake_seed[12] = {'B','S','S','D', 0x39,0x05,0,0, 0,0,0,0};
	writeRaw("seed.bin", fake_seed, sizeof fake_seed);
	const uint8_t fake_region[16] = {'B','S','R','1', 1,0,0,0, 1,0,0,0, 0,0,0,0};
	writeRaw("r.0.0.bsr", fake_region, sizeof fake_region);

	uint64_t got = 12345;
	const DayTimeStatus st = dayNightRead(g_dir, &got);
	check(st == DAYTIME_ABSENT,
	      "a pre-v1.8.9 world with no time.bin reads as ABSENT, not as damaged");
	check(got == DAY_START_TICKS,
	      "and resolves to DAY_START_TICKS - the world opens in the morning");

	DayNight d;
	dayNightInit(&d, got);
	check(dayNightPhase(dayNightTimeOfDay(&d)) == DAY_PHASE_DAY,
	      "an old save opens in daytime");
	check(dayNightLevel(dayNightTimeOfDay(&d)) == 1.0f,
	      "at exactly the brightness it had before this module existed - dayLevel 1.0, which "
	      "is what chunk_render.c pins the uniform to today");

	// And the neighbouring sidecars are untouched by all of this.
	char path[512];
	snprintf(path, sizeof path, "%s/seed.bin", g_dir);
	FILE *f = fopen(path, "rb");
	uint8_t back[16];
	size_t n = 0;
	if (f) { n = fread(back, 1, sizeof back, f); fclose(f); }
	check(n == 12 && memcmp(back, fake_seed, 12) == 0,
	      "the seed sidecar beside it is byte-identical afterwards - no existing format was "
	      "touched");
}

static void testSidecarDamageIsHarmless(void)
{
	uint64_t got;

	// Every way of being wrong, and every one of them a morning rather than a refusal.
	const uint8_t bad_magic[16] = {'X','X','X','X', 1,0,0,0,0,0,0,0, 0,0,0,0};
	writeRaw(DAY_TIME_FILE, bad_magic, sizeof bad_magic);
	got = 999;
	check(dayNightRead(g_dir, &got) == DAYTIME_DAMAGED, "a wrong magic is DAMAGED");
	check(got == DAY_START_TICKS, "and still hands back a usable morning");

	// A good record with one payload byte flipped: the CRC is the only thing that can catch
	// this, so it is the check that proves the CRC is actually computed.
	removeSidecar();
	check(dayNightWrite(g_dir, 24000ull * 3ull + 15000ull), "write a good record");
	char path[512];
	snprintf(path, sizeof path, "%s/%s", g_dir, DAY_TIME_FILE);
	FILE *f = fopen(path, "r+b");
	check(f != NULL, "reopen it for patching");
	if (f) {
		fseek(f, 5, SEEK_SET);
		const uint8_t flip = 0xA5;
		fwrite(&flip, 1, 1, f);
		fclose(f);
	}
	got = 999;
	check(dayNightRead(g_dir, &got) == DAYTIME_DAMAGED,
	      "a single flipped payload byte fails the CRC");
	check(got == DAY_START_TICKS, "and is a morning, not a wrong evening");

	// Short.
	const uint8_t short_rec[9] = {'B','S','T','M', 1,0,0,0,0};
	writeRaw(DAY_TIME_FILE, short_rec, sizeof short_rec);
	check(dayNightRead(g_dir, &got) == DAYTIME_DAMAGED, "a short record is DAMAGED");

	// Long. Nothing this build writes produces one, so something else wrote here.
	uint8_t long_rec[20];
	memset(long_rec, 0, sizeof long_rec);
	memcpy(long_rec, "BSTM", 4);
	writeRaw(DAY_TIME_FILE, long_rec, sizeof long_rec);
	check(dayNightRead(g_dir, &got) == DAYTIME_DAMAGED,
	      "a record with trailing bytes is DAMAGED");

	// Empty.
	writeRaw(DAY_TIME_FILE, "", 0);
	check(dayNightRead(g_dir, &got) == DAYTIME_DAMAGED, "an empty file is DAMAGED");

	removeSidecar();
}

static void testSidecarArgumentSafety(void)
{
	uint64_t got = 999;

	check(dayNightRead(NULL, &got) == DAYTIME_NO_WORLD_DIR,
	      "a NULL world directory is NO_WORLD_DIR - a joined session, not a fault");
	check(got == DAY_START_TICKS, "and still writes the default through");
	check(dayNightRead("", &got) == DAYTIME_NO_WORLD_DIR, "an empty one is too");

	check(!dayNightWrite(NULL, 5), "dayNightWrite refuses a NULL directory");
	check(!dayNightWrite("", 5),   "and an empty one");

	// *out may be NULL - a caller that only wants the status.
	removeSidecar();
	check(dayNightRead(g_dir, NULL) == DAYTIME_ABSENT,
	      "dayNightRead tolerates a NULL out pointer");

	// A path too long to hold "<dir>/time.bin" is REFUSED at both ends rather than silently
	// truncated into a neighbouring file's name. world/worldseed.c measured that exact bug in
	// its own reader; this is the same guard, checked rather than assumed.
	char huge[400];
	memset(huge, 'a', sizeof huge - 1);
	huge[sizeof huge - 1] = '\0';
	check(!dayNightWrite(huge, 5),
	      "a world directory too long for the path buffer refuses the write");
	got = 999;
	check(dayNightRead(huge, &got) == DAYTIME_DAMAGED,
	      "and reads as DAMAGED rather than as 'no sidecar'");
	check(got == DAY_START_TICKS, "still with a usable default");
}

// ── The check-count pin ──────────────────────────────────────────────────────────────

// A checker that has never been red is an untested function running at the worst possible
// moment. This one was made to fail on purpose before it was trusted, and it was not a
// contrived red: the number below was wrong on the first run and the pin said so — "FAIL CHECK
// COUNT: expected 152, ran 233 — 81 check(s) APPEARED".
#define EXPECTED_CHECKS 234

static void checkCountPin(void)
{
	const int ran = g_checks;
	g_checks++;
	if (ran != EXPECTED_CHECKS) {
		g_fails++;
		printf("FAIL  CHECK COUNT: expected %d, ran %d — %d check(s) %s\n",
		       EXPECTED_CHECKS, ran, abs(EXPECTED_CHECKS - ran),
		       ran < EXPECTED_CHECKS ? "WENT MISSING" : "APPEARED");
	}
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	if (argc < 2) {
		printf("usage: daynight_test <writable-dir>\n");
		return 2;
	}
	snprintf(g_dir, sizeof g_dir, "%s", argv[1]);

	puts("== daynight test ==");

	testConstantsPinned();
	testPhaseBoundaries();
	testWikiBoundariesBothSides();
	testWikiTableIsAPartitionAndMatches();
	testCurveIsNotASine();
	testFloatFormulaAgainstTable();
	testCelestialAngle();
	testSkyColour();
	testCounterAndWrap();
	testDeterminism();
	testSidecarRoundTrip();
	testOldSaveStillLoads();
	testSidecarDamageIsHarmless();
	testSidecarArgumentSafety();

	checkCountPin();

	printf("\n%s %d checks, %d failed\n", g_fails == 0 ? "PASS" : "FAIL",
	       g_checks, g_fails);
	return g_fails == 0 ? 0 : 1;
}

#endif /* !__3DS__ */
