# Blocksmith v1.8.4 — New 3DS

A New 3DS has been running this game as if it were an Old 3DS. Same 268 MHz clock,
no L2 cache, one usable CPU core, and a memory ceiling set for a console with half
the RAM. This release is the one that asks the hardware for what it actually has.

It also settles two things that came back from playing 1.8.3: you could walk out of
water like it wasn't there, and a failed download made you go all the way round the
houses to try again.

## The New 3DS is now treated as a New 3DS

The game asks the system for the **804 MHz clock** and the **L2 cache** at boot, and
the title's permissions were rewritten to allow both, plus the larger memory mode and
access to the third CPU core.

Background work — world generation, region loading, lighting and saving — now runs on
**core 2**, which exists only on a New 3DS and which nothing else on the console uses.

**An Old 3DS is unchanged in every respect.** It runs at the clock it always did and
takes none of these paths. Its background work stays on core 0, because the only other
core there belongs to the operating system, and the measurement that proved taking it
makes the world fill 2.5× slower still stands.

## You have to press jump to get out of water

Walking into the bank no longer lifts you onto it. You stay in the water, bobbing
against the shore, until you press jump — and then you climb out the same way you
always did, with the same ledge rules and the same headroom check.

- A press stays armed for a quarter of a second, so pressing jump slightly *before*
  you reach the bank still works.
- One press buys one climb. It is spent on the way up, so a single tap cannot carry
  you up a staircase of banks.
- Water is not a trap. If your feet have already reached dry land you walk up an
  ordinary one-block step exactly as before.

## A RETRY DOWNLOAD button

When a download fails, the button now says what it will do and does it: it goes
straight back to downloading the release it already found.

Before this, every failure alike offered `CHECK NOW`, so recovering from a dropped
transfer meant asking GitHub for the release list again, being told again that a newer
version exists, and only then downloading. Three presses and two round trips to retry
one thing that was already decided.

A failed *check* still offers `CHECK NOW`, because there genuinely is nothing else to
retry.

## Fixed

- **The New 3DS core request was written but unreachable.** The code that picks a core
  was gated behind the build flag that guards the *system* core, and that flag is off
  by default — for a good, measured reason that has nothing to do with core 2. The
  result was that a New 3DS asked for, and got, exactly what an Old 3DS got. The two
  questions are now separate.

## Compatibility

Worlds, saves and multiplayer are untouched. Nothing in the world format, the
generator or the network protocol changed, so a 1.8.4 client and a 1.8.3 client see
the same world and can play together.

## Verified

- World self-test passes at 5477 checks; with the water gate deleted it goes red at
  5 checks, starting with the body climbing the bank without a press.
- Retry button: 45 checks pass; collapsed back to 1.8.3 behaviour, 3 go red.
- Core ladder: 52 checks, proved red four separate ways.
- Full host suite green. Console binary builds clean and boots to a live title screen
  at 60 FPS.

## Not verified

The clock, cache, memory mode and core-2 grant are hardware requests that no emulator
models, and nothing in this project has run on real hardware since 1.2.5. What is
proved is that the ladder returns the right cores and that every refusal falls back to
what the console did before. Whether a New 3DS actually grants them has to be seen on
a New 3DS.
