#include "world/chest.h"

#include <string.h>

void chestStateInit(ChestState* cs)
{
	memset(cs, 0, sizeof(*cs));
}

void chestStatePack(const ChestState* cs, uint8_t out[BLOCKSTATE_PAYLOAD_BYTES])
{
	// No reserved tail to zero first — CHEST_PAYLOAD_USED_BYTES == BLOCKSTATE_PAYLOAD_BYTES
	// (see the header), so the loop below writes every byte blockStateSet will store. The
	// furnace's own pack memsets first because it leaves five bytes unwritten; this one
	// doesn't, and skipping the memset here is that fact made visible rather than a missed
	// step.
	for (int i = 0; i < CHEST_SLOTS; i++) {
		out[i * 2]     = (uint8_t)cs->item[i];
		out[i * 2 + 1] = cs->count[i];
	}
}

void chestStateUnpack(ChestState* cs, const uint8_t in[BLOCKSTATE_PAYLOAD_BYTES])
{
	for (int i = 0; i < CHEST_SLOTS; i++) {
		cs->item[i]  = (ItemId)in[i * 2];
		cs->count[i] = in[i * 2 + 1];
	}
}
