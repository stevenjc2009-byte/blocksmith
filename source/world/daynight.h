// v1.8.9. The day/night clock, and everything the sun's position decides.
//
// ── What this is, and what it deliberately is not ─────────────────────────────────────
//
// Before this file there was no clock of any kind in the world: no time of day, no day
// counter, and scene/chunk_render.c pinned the shader's `dayLevel` uniform to 1.0 with the
// comment "until a day/night cycle exists". This is that cycle. It is one 64-bit tick counter
// and a handful of pure functions of it, and it owns NOTHING else — it does not draw, it does
// not touch the GPU, it does not know what a chunk is, and it never calls back into the world.
//
// It is deliberately NOT a mob spawner, a weather system or a sleep mechanic. v1.8.16 owns
// mobs; the one hook this file offers that side is dayNightSkyLight(), which is the value the
// spawn rule will be written against when it exists.
//
// ── The rate is not this file's to choose ────────────────────────────────────────────
//
// Minecraft runs at 20 ticks per second and so does this project: world/tick.h's TickClock is
// already the fixed-step 20 TPS clock that fluid spread, redstone and crop growth are phased
// against, and it is already shared verbatim by the console client, the Linux dedicated server
// and the host suite. This file therefore does not have a clock in it. It has a COUNTER, and
// whoever owns the TickClock feeds it:
//
//     const int n = tickClockAdvance(&s_tickclock, elapsed_us);
//     dayNightAdvance(&s_daynight, n);
//
// That is the whole integration, and it is why the cycle is automatically the right length on
// a console holding 59.83 fps, on a console struggling at 30, and on a headless server: none
// of them is the clock, tick.h is.
//
// ── The numbers, and where they come from ────────────────────────────────────────────
//
// Every constant below is Minecraft Java Edition's, read off minecraft.wiki rather than
// recalled. The citations are on the constants themselves. The two that shape everything:
//
//   * https://minecraft.wiki/w/Daylight_cycle — "The daylight cycle is a 20-minute-long cycle
//     between two main light settings: day and night", 24,000 ticks, and the four phase
//     boundaries at 0 / 12000 / 13000 / 23000.
//   * https://minecraft.wiki/w/Light — the "Internal sky light" table, which is the tick-by-
//     tick ground truth this file's curve is checked against in world/daynight_test.c.
//
// ── The curve is NOT a sine, and that is the whole point ─────────────────────────────
//
// A naive fade would darken the world continuously from noon to midnight. Minecraft does not
// do that and it is very visibly not what the game looks like: the sky sits at FULL brightness
// for the whole of the day, drops over about ninety seconds at dusk, sits at FULL dark for the
// whole of the night, and comes back over about ninety seconds at dawn. That shape comes from
// a clamp, not from a curve — see dayNightSkyDarkenF() below, where cos() is clamped to
// ±0.25 before it is used, so the cosine is saturated for most of the cycle and only live
// inside two narrow windows.
//
// ── Determinism, and why there are two brightness functions ──────────────────────────
//
// dayNightSkyDarken() returns an INTEGER and is the gameplay-facing answer. It is a lookup in
// a table of pinned boundary ticks, so it is bit-identical on x86, on ARM11 and on whatever
// the dedicated server is built on. That matters because it is the value a future mob-spawn
// rule reads, and a spawn rule that disagrees between a client and its server is a desync.
//
// dayNightSkyDarkenF() returns a FLOAT from the real cosine formula and is the RENDERING
// answer. It feeds dayNightLevel(), which is the `dayLevel` uniform, and nothing else. A last-
// ulp difference there costs at most an invisible fraction of one light level on one frame.
//
// The two are checked against each other at all 24,000 ticks in world/daynight_test.c, and the
// handful of ticks where they disagree are named there rather than smoothed over: Minecraft
// computes its cosine from a 65,536-entry lookup table (Mth.cos), not from a true cosine, so
// its published boundary ticks are one tick off a true cosine's in a couple of places.
//
// ── No <3ds.h>, and that is load-bearing ─────────────────────────────────────────────
//
// Same reasoning as world/tick.h and world/worldseed.h: this file is compiled into the console
// client and the host test suite from the same source, and is a candidate for the dedicated
// server's vendored world/ set when time goes on the wire. The SD card is a devoptab under
// "sdmc:/", so the fopen/fread in the sidecar half below reach it on console exactly as they
// reach a host temp directory under test — which is what lets world/daynight_test.c link this
// real file rather than a copy of its logic.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// ── The cycle ────────────────────────────────────────────────────────────────────────

// https://minecraft.wiki/w/Daylight_cycle: "The daylight cycle is a 20-minute-long cycle
// between two main light settings: day and night", and "in Minecraft, time is 72 times faster
// than normal time". 24,000 ticks at world/tick.h's TICK_HZ of 20 is 1,200 seconds, which is
// those 20 minutes exactly. The two files agree by arithmetic and not by coincidence, and the
// static assert in daynight.c says so.
#define DAY_TICKS  24000u

// The named times, all from https://minecraft.wiki/w/Daylight_cycle. The phase boundaries come
// from that page's Daytime / Sunset / Nighttime / Sunrise sections; DAY_TICK_DAY is the wiki's
// own `/time set day` value, quoted verbatim there as "Sets the time to 1000".
//
//   Daytime   0 .. 11999    10 min        (mid: 6000, noon / 12:00)
//   Sunset    12000 .. 12999    50 sec
//   Night     13000 .. 22999    8 min 20 sec  (mid: 18000, midnight / 00:00)
//   Sunrise   23000 .. 23999    50 sec
//
// Tick 0 is 06:00 in the in-game clock, which is why dawn is 0 and noon is 6000 rather than
// the other way round.
#define DAY_TICK_DAWN      0u       // 06:00, the first tick of the cycle
#define DAY_TICK_DAY    1000u       // `/time set day`
#define DAY_TICK_NOON   6000u       // `/time set noon`, 12:00
#define DAY_TICK_SUNSET 12000u      // 18:00, sunset begins
#define DAY_TICK_NIGHT  13000u      // 19:00, `/time set night`
#define DAY_TICK_MIDNIGHT 18000u    // 00:00, `/time set midnight`
#define DAY_TICK_SUNRISE  23000u    // 05:00, `/time set sunrise`

// What a brand-new world starts at. Minecraft's own answer: a new world begins at morning, and
// the wiki gives 1000 as the tick `/time set day` names. Starting at 0 instead would put the
// player one tick after dawn with the sky still coming up, which is a worse first frame and is
// not what the game does.
#define DAY_START_TICKS  DAY_TICK_DAY

// ── Sky light ────────────────────────────────────────────────────────────────────────

// https://minecraft.wiki/w/Light, "Internal sky light": at noon in clear weather the internal
// sky light level is 15, and at midnight in any weather it is 4. Never 0 — a moonlit surface
// in Minecraft is dim, not black, and a cycle that took it to zero would be darker than the
// game it is copying.
#define DAY_SKYLIGHT_MAX  15
#define DAY_SKYLIGHT_MIN  4

// The subtraction that produces those two: 15 - 11 = 4. Minecraft's getSkyDarken returns
// 0 at noon and 11 at midnight, and this is that 11.
#define DAY_SKYDARKEN_MAX 11

typedef enum {
	DAY_PHASE_DAY = 0,
	DAY_PHASE_SUNSET,
	DAY_PHASE_NIGHT,
	DAY_PHASE_SUNRISE,
} DayPhase;

// ── The counter ──────────────────────────────────────────────────────────────────────

// Total ticks since the world was created, NOT ticks within a day. Two reasons it is the whole
// count and not a 0..23999 wrap:
//
//   * the day NUMBER falls out of it for free, and Minecraft has one (the wiki's Moon phases
//     section: "Each day that progresses adds 24000 ticks to the time counter"), so a world
//     that wants to say "day 14" already can;
//   * an 8-day moon phase is `(ticks / 24000) % 8`, which needs the total and cannot be
//     recovered from a wrapped value.
//
// uint64_t rather than uint32_t is not caution for its own sake: at 20 TPS a uint32 overflows
// after 6.8 real years of continuous play, which is admittedly nobody — but the field is
// written to disk, and a saved counter that can wrap is a saved counter that eventually reads
// back as a world that is younger than it is. Eight bytes on disk costs nothing.
typedef struct {
	uint64_t ticks;
} DayNight;

// Starts the clock at `ticks`. Pass DAY_START_TICKS for a brand-new world, or whatever
// dayNightRead() returned for one being reloaded.
void dayNightInit(DayNight* d, uint64_t ticks);

// Advances by `n` ticks — exactly the int that world/tick.h's tickClockAdvance() returned, so
// the cycle runs on the simulation clock and not on the frame rate. A negative or zero `n`
// advances nothing: a clock that appears to run backwards must never rewind the day, for the
// same reason tickClockAdvance refuses negative elapsed time.
void dayNightAdvance(DayNight* d, int n);

// Sets the counter outright. This is the SERVER-AUTHORITATIVE and DEBUG entry point, and it is
// the only way time ever moves other than forward. See the multiplayer note at the bottom of
// this header.
void dayNightSet(DayNight* d, uint64_t ticks);

uint64_t dayNightTicks(const DayNight* d);

// Ticks within the current day, 0 .. DAY_TICKS-1. This is the argument every function below
// takes, so they are all pure functions of a small integer and need no struct to be tested.
uint32_t dayNightTimeOfDay(const DayNight* d);

// Whole days since the world was created. Day 0 is the first day.
uint64_t dayNightDay(const DayNight* d);

// The 8-day lunar cycle, 0..7. https://minecraft.wiki/w/Daylight_cycle: "the moon appears in
// one of eight different phases each night". Nothing in this version reads it; it is here
// because it is one line and because the counter above was made 64-bit to make it possible.
unsigned dayNightMoonPhase(const DayNight* d);

// ── Pure functions of the time of day ────────────────────────────────────────────────
//
// All of these take a raw 0..23999 tick. Values outside that range are reduced modulo
// DAY_TICKS rather than rejected, so a caller that hands over a whole counter by mistake gets
// the right answer instead of a fault.

// Which of the four named phases `tod` falls in.
DayPhase dayNightPhase(uint32_t tod);

// The celestial angle in ROTATIONS, 0.0 at noon, 0.5 at midnight. This is Minecraft's
// getTimeOfDay, and it is the wiki's Sky angle formula:
//
//     alpha = (1 - cos(pi * mod_1((t-6000)/24000)) + mod_4((t-6000)/6000)) * 60 degrees
//
// with 0 degrees being noon (https://minecraft.wiki/w/Daylight_cycle, "Sky angle"). Dividing
// that by 360 gives exactly what this returns, and daynight.c shows the algebra.
//
// The sun does NOT move at a constant rate: the formula is a linear ramp plus a cosine, so the
// rotation rate varies by a factor of about 1.78 across the cycle.
//
// A CONTRADICTION IN THE SOURCE, recorded rather than resolved. The same wiki section's prose
// says the sky "move[s] faster during noon and midnight and slower during sunrises and
// sunsets". Its own formula does not do that, and neither does the game's timeOfDay function
// that the formula reduces to. Differentiating either gives 2/3 + pi*sin(pi*d)/6, which is at
// its MINIMUM at noon (d = 0) and its MAXIMUM at midnight (d = 0.5). MEASURED on this
// implementation, as rotations per 100 ticks:
//
//     noon      0.0027921      <- the slowest point in the cycle, not a fast one
//     dawn      0.0043103
//     dusk      0.0043305
//     midnight  0.0049594      <- the fastest
//
// So the prose is half right: midnight is fast. It is wrong about noon, which is the slowest
// moment of the whole day. This file follows the FORMULA, because the formula is what the game
// implements and because it is what the sky-light table on the Light page is consistent with.
// world/daynight_test.c pins the ordering above so the choice cannot be silently reversed.
//
// Returns a value in [0,1). It wraps at NOON, not at dawn, because 0 rotations is noon.
float dayNightCelestialAngle(uint32_t tod);

// Minecraft's getSkyDarken as a float, 0.0 at noon rising to 11.0 at midnight. RENDERING ONLY
// — see the header note about the two brightness functions. Everything the cycle looks like
// comes from here.
float dayNightSkyDarkenF(uint32_t tod);

// Minecraft's getSkyDarken as an integer, 0..11. GAMEPLAY. Deterministic on every platform,
// because it is a table lookup rather than a cosine.
int dayNightSkyDarken(uint32_t tod);

// The internal sky light level at `tod` in clear weather, DAY_SKYLIGHT_MIN..DAY_SKYLIGHT_MAX.
// Exactly 15 - dayNightSkyDarken(tod), and it is spelled out as its own function because it is
// the number https://minecraft.wiki/w/Light publishes and the number a spawn rule will read.
int dayNightSkyLight(uint32_t tod);

// THE UNIFORM. What scene/chunk_render.c must write into `dayLevel` every frame.
//
// The shader (source/shaders/world_dynamic.v.pica) computes
//
//     lum = max(sky * dayLevel, block)
//
// with `sky` already normalised to 0..1 by dividing the vertex's sky nibble by 15. So a value
// of L/15 here makes a fully sky-lit face render at internal light level L, which is precisely
// what the wiki's table is stating. The range is therefore 4/15 (0.2667) at night to 1.0 in
// full day, and 1.0 is byte-for-byte the value the uniform is pinned at today — so a build
// that computes this and never advances the clock draws exactly the frame it draws now.
float dayNightLevel(uint32_t tod);

// The sky COLOUR scale, 0..1. Minecraft's own: clamp(cos(angle * 2pi) * 2 + 0.5, 0, 1),
// multiplied into the biome's daytime sky colour. https://minecraft.wiki/w/Sky states the
// endpoint verbatim — "At night, the sky color is always #000000, and during sunset and
// sunrise this gradually fades with the biome-dependent daytime colors" — and this function is
// that fade.
//
// It saturates on the SAME window the light curve does, because both clamp the same cosine at
// the same +-0.25: the sky goes black over exactly the span the ground goes dark.
float dayNightSkyTint(uint32_t tod);

// The two byte orders scene/chunk_render.h's SKY_CLEAR_RGBA8 and SKY_FOG_BGR already keep, with
// dayNightSkyTint applied. Two functions rather than one because the two GPU registers disagree
// about byte order and that is exactly the drift chunk_render.h's comment warns about — so the
// scaling is done once, here, from the ONE base constant, rather than twice at two call sites.
//
// `base_rgba8` is SKY_CLEAR_RGBA8. The alpha byte is passed through unscaled.
uint32_t dayNightSkyClearRgba8(uint32_t tod, uint32_t base_rgba8);

// Same input — SKY_CLEAR_RGBA8, not SKY_FOG_BGR — and the byte swap is done here. Taking the
// RGBA8 constant for both is deliberate: it makes it impossible for a caller to scale the fog
// from one base and the clear from another, which is the drift that puts a grey sheet in front
// of the sky.
uint32_t dayNightSkyFogBgr(uint32_t tod, uint32_t base_rgba8);

// ── The sidecar ──────────────────────────────────────────────────────────────────────
//
// A world reloaded at dusk must come back at dusk, so the counter is saved. It is a NEW file
// beside seed.bin and genver.bin, written in the identical magic/payload/CRC-32 shape those
// two use (world/worldseed.c, world/genversion.c), and that choice is the whole of this
// module's save-format impact:
//
//   * NO existing format version is bumped, because no existing file is touched. region.bsr's
//     REGION_VERSION, genver.bin's version word and seed.bin's layout are all untouched.
//   * An OLD save has no time.bin. That is DAYTIME_ABSENT, which is not a fault — it is what
//     every world made before v1.8.9 looks like — and it resolves to DAY_START_TICKS. Old
//     worlds open, in the morning, exactly as they did before this file existed.
//
// ── Why a damaged time.bin does NOT refuse the world ─────────────────────────────────
//
// This is a deliberate departure from world/worldseed.h, which REFUSES on a damaged sidecar,
// and the asymmetry is the point. A wrong seed regenerates the landscape under a base the
// player has already built and there is no recovering from it, so worldseed.c stops. A wrong
// time of day makes it morning when it should be evening. Nothing is lost and nothing is
// overwritten; the next save writes the correct value back.
//
// Refusing entry to a world over an unreadable four-byte clock would be a strictly worse
// outcome than the fault it is protecting against, so this module reports DAYTIME_DAMAGED and
// hands back DAY_START_TICKS. The status is still distinct from ABSENT so a caller that wants
// to say something about it can.
//
// The other reason the two files differ: seed.bin is written ONCE in a world's life, so its
// torn-write window is one moment. time.bin is written on every save, so its window is every
// save — which is survivable only because the failure is harmless. It would not have been an
// acceptable design for the seed.

#define DAY_TIME_FILE  "time.bin"

// 4 magic + 8 ticks (little-endian) + 4 CRC-32 over the first 12. Fixed size, so a short read
// is itself a detectable fault rather than something to parse around — world/worldseed.h's
// reasoning, and its shape, with a wider payload.
#define DAY_TIME_BYTES 16

typedef enum {
	// A usable counter. *out holds it.
	DAYTIME_OK = 0,

	// No sidecar in this directory. A world made before v1.8.9, or one that has never been
	// saved. *out holds DAY_START_TICKS and the caller should use it.
	DAYTIME_ABSENT,

	// A sidecar is present and cannot be trusted — wrong magic, failed CRC, short, long, or a
	// path that would not fit the buffer. *out holds DAY_START_TICKS and the caller should use
	// it. See the note above for why this is not a refusal.
	DAYTIME_DAMAGED,

	// No world directory at all — a joined server session, where the time comes off the wire
	// rather than off this console's card. *out holds DAY_START_TICKS.
	DAYTIME_NO_WORLD_DIR,
} DayTimeStatus;

// Writes "<world_dir>/time.bin". False on any IO failure, including a path that does not fit.
// A refused write is a world that reopens in the morning; it is never a world that opens wrong.
bool dayNightWrite(const char* world_dir, uint64_t ticks);

// Reads it back. *out is ALWAYS written — DAY_START_TICKS on every non-OK status — so a caller
// that ignores the status still gets a usable world rather than an uninitialised one. `out`
// may be NULL if only the status is wanted.
DayTimeStatus dayNightRead(const char* world_dir, uint64_t* out);

// ── Multiplayer, and why nothing here has to be torn up to add it ────────────────────
//
// Time of day is server-authoritative in any sane design, and this module is already shaped
// for that without implementing it. The whole of the client's state is one uint64 and the only
// non-monotonic way to move it is dayNightSet(), so making a session authoritative is a matter
// of calling that from a packet handler.
//
// The wire design, for whoever lands it:
//
//   * BS_PROTO_VERSION stays 1. A new SERVER-TO-CLIENT opcode inside BS_PKT_DATA is safe
//     client-first, because net/networld.c's networldApplyPayload switch ends in
//     `default: break;` and an old client simply ignores a message it does not know. The
//     reverse is NOT safe — the server's handle_app_payload switch ends in send_kick() — so
//     this must never become a client-to-server message.
//   * The highest opcode currently defined in deps/blocksmith-server/proto/bs_proto.h's
//     enum bs_app_msg is BS_APP_WORLD_GEN = 0x0F. 0x10 is free; call it BS_APP_TIME_SYNC.
//   * Payload: the uint64 counter, little-endian, 8 bytes. Not a time-of-day — the whole
//     counter, so day number and moon phase agree across the room too.
//   * Cadence: once on join (so a player who arrives at dusk arrives at dusk), and then
//     periodically. tickDue(t, TICK_HZ, 0) is once a second and is already the idiom the
//     server uses for its position broadcast; the drift a client can accumulate in one second
//     is at most the ticks its own TickClock dropped, which is exactly what needs correcting.
//   * The client applies it with dayNightSet(). No interpolation is needed at one second: a
//     correction of a few ticks moves dayLevel by less than a thousandth.
//
// The one thing that would have to be torn up if it were built differently — a client that
// derived time from its own wall clock rather than from a counter — is precisely what this
// module does not do.
