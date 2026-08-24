// Break-time arithmetic. See mining.h for the contract and for why the tool table is empty.
#include "world/mining.h"

uint32_t miningCeilDiv(uint32_t num, uint32_t den)
{
	if (num == 0) return 0;
	if (den == 0) den = 1;   // see mining.h: no sane answer, and trapping is worse
	// (num + den - 1) / den, written so the intermediate cannot overflow a uint32_t. num is
	// at most 255 * 256 here and den at most a multiplier, so the guarded form is belt and
	// braces rather than necessary — but this helper is public and task 32 will feed it
	// numbers this file cannot see.
	const uint32_t q = num / den;
	return (q * den == num) ? q : q + 1;
}

uint32_t miningSpeedMultiplier(ItemId holding)
{
	switch (holding) {
	// ── Task 32 adds its tool rows HERE, one case each. ──
	//
	// Nothing else. Every id in this game is a block (world/inventory.h: ItemId IS BlockId),
	// so holding planks is holding nothing, in mining terms, exactly as holding air is.
	default:
		return MINING_SPEED_ONE;   // bare hands, 1x
	}
}

uint32_t breakTicksRequired(BlockId broken, ItemId holding)
{
	const uint32_t hardness = blockHardnessTicks(broken);
	if (hardness == 0) return 0;   // no break time at all: air, water

	const uint32_t speed = miningSpeedMultiplier(holding);

	// The one division in the module, paid once when a break starts. Scaling the numerator
	// by MINING_SPEED_ONE first is what keeps this integer: hardness / (speed/ONE) is
	// (hardness * ONE) / speed.
	const uint32_t ticks = miningCeilDiv(hardness * MINING_SPEED_ONE, speed);

	// A block with hardness is never instant. Unreachable at 1x — ceil() of anything over 1
	// is already >= 1 — and deliberately kept anyway, because the first tool task 32 adds
	// that is faster than the softest block in the registry makes it reachable, and the
	// place to state "a break takes a press" is here rather than in whichever timer notices.
	return ticks < 1u ? 1u : ticks;
}
