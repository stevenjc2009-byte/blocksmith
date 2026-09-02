#include "world/genretry.h"

#include <string.h>

// Floor-mod, same shape as main.c's genWrap and for the same reason: a negative column has to
// land inside the window, not off the front of it.
static int retryWrap(int32_t c, int span)
{
	const int m = (int)(c % span);
	return m < 0 ? m + span : m;
}

static GenRetrySlot* retrySlotFor(GenRetryLedger* h, int32_t cx, int32_t cz)
{
	return &h->slots[retryWrap(cz, h->span) * h->span + retryWrap(cx, h->span)];
}

bool genRetryInit(GenRetryLedger* h, int span)
{
	if (span <= 0 || span > GEN_RETRY_SLOTS || span * span > GEN_RETRY_SLOTS) return false;

	memset(h->slots, 0, sizeof(h->slots));
	h->span = span;
	h->outstanding = 0;
	return true;
}

void genRetryMark(GenRetryLedger* h, int32_t cx, int32_t cz)
{
	GenRetrySlot* s = retrySlotFor(h, cx, cz);
	const bool continuing = s->set && s->cx == cx && s->cz == cz;

	if (continuing) {
		if (s->attempts < GEN_RETRY_MAX_ATTEMPTS) s->attempts++;
	} else {
		// Either this slot was empty, or it held a different column — which can only be a
		// stale entry the ring has already moved past (the wrap is exactly GEN_AREA_SPAN
		// wide, same argument main.c's own s_col_in comment makes), so overwriting it here
		// costs nothing that genRetryTick's own radius check was not already going to drop.
		s->cx = cx;
		s->cz = cz;
		s->attempts = 0;
		h->outstanding++;
	}

	s->set = true;
	s->inflight = false;

	int frames = GEN_RETRY_BASE_FRAMES << s->attempts;
	if (frames <= 0 || frames > GEN_RETRY_MAX_FRAMES) frames = GEN_RETRY_MAX_FRAMES;
	s->cooldown = (int16_t)frames;
}

bool genRetryClear(GenRetryLedger* h, int32_t cx, int32_t cz)
{
	GenRetrySlot* s = retrySlotFor(h, cx, cz);
	if (!s->set || s->cx != cx || s->cz != cz) return false;

	s->set = false;
	s->inflight = false;
	h->outstanding--;
	return true;
}

void genRetryTick(GenRetryLedger* h, int32_t center_cx, int32_t center_cz, int radius,
                   bool (*submit)(void* ud, int32_t cx, int32_t cz), void* ud)
{
	for (int i = 0; i < h->span * h->span; i++) {
		GenRetrySlot* s = &h->slots[i];
		if (!s->set || s->inflight) continue;

		const int32_t dx = s->cx - center_cx, dz = s->cz - center_cz;
		if (dx < -radius || dx > radius || dz < -radius || dz > radius) {
			// The ring moved past this column before its backoff elapsed. main.c's
			// genInstallOne already drops the common case (the failed column's own late
			// arrival lands outside genInArea and is handled there); this is the case
			// where nothing else would ever have visited this slot again.
			s->set = false;
			h->outstanding--;
			continue;
		}

		// Decrement first, then test: a mark of GEN_RETRY_BASE_FRAMES retries exactly
		// GEN_RETRY_BASE_FRAMES ticks later, not one after — the two-statement form
		// (test, decrement-and-continue) would make the countdown one tick longer than
		// the constant it is named after, which is the kind of off-by-one that is easy to
		// get wrong reading this function and easy to get right testing it, so it is
		// tested (genretry_test.c's not-spin checks pin the exact tick count).
		if (s->cooldown > 0) s->cooldown--;
		if (s->cooldown > 0) continue;

		if (submit && submit(ud, s->cx, s->cz)) s->inflight = true;
		// else: the job ring was full this one frame — leave cooldown at 0 so this is
		// retried on the very next call instead of waiting out a whole backoff period for
		// what is a transient, not a generation, refusal.
	}
}

int genRetryOutstanding(const GenRetryLedger* h)
{
	return h->outstanding;
}
