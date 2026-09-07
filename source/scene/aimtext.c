#include "scene/aimtext.h"

#include <string.h>

void aimTextInit(AimText* t)
{
	memset(t, 0, sizeof *t);   // all-zero is the documented initial state — see the header
}

// Whether `id` has a name worth showing. Three refusals, each a distinct way to put a wrong
// string on screen:
//   air         — never targetable (raycast stops on solids), but a caller passing the id
//                 of a miss must not see "air";
//   undefined   — registryGet() answers the air row for these rather than NULL, so without
//                 this gate a stale or corrupt id would ALSO read "air";
//   empty name  — registryRegister() refuses one but registryDefUnpack() checks only that a
//                 NUL exists, so a DEFS record from a server can install a row whose name is
//                 "", and a label of "" is a bare backing pill under the reticle with nothing
//                 in it.
static bool nameable(BlockId id)
{
	if (id == BLOCK_AIR) return false;
	if (!registryIsDefined(id)) return false;
	return registryGet(id)->name[0] != '\0';
}

bool aimTextUpdate(AimText* t, bool targeted, BlockId id, uint32_t now_ms)
{
	if (targeted && nameable(id)) {
		if (id != t->cached) {
			// Bounded copy, terminator forced. The registry already guarantees a NUL inside
			// REGISTRY_NAME_MAX (registry.c's copyName and registryDefUnpack), so this is belt
			// over the table's braces: the one string this file ever emits cannot run past its
			// own buffer even if that guarantee is broken somewhere upstream one day.
			const char* name = registryGet(id)->name;
			size_t i;
			for (i = 0; i + 1 < sizeof t->text && name[i] != '\0'; i++)
				t->text[i] = name[i];
			t->text[i] = '\0';

			t->cached = id;
			t->formats++;
		}

		// Whatever it was doing — hidden, mid-fade, already shown — a live target is full
		// opacity from this frame on. A fade that was under way is simply abandoned.
		t->state = AIMTEXT_SHOWN;
		t->alpha = 255;
		return true;
	}

	// No nameable target this frame. The cached text is left alone on purpose (see the header
	// on cost): what changes is only how visible it is.
	if (t->state == AIMTEXT_SHOWN) {
		t->state      = AIMTEXT_FADING;
		t->lost_at_ms = now_ms;   // the ramp starts NOW, at 255 — the label does not blink
	}

	if (t->state == AIMTEXT_FADING) {
		const uint32_t gone = now_ms - t->lost_at_ms;   // unsigned: correct across a wrap
		if (gone >= AIMTEXT_FADE_MS) {
			t->state = AIMTEXT_HIDDEN;
			t->alpha = 0;
		} else {
			// Linear, 255 at gone == 0 down to 1 at gone == FADE_MS - 1; the 0 is the arm
			// above, so alpha is never 0 while the state still says FADING. 255 * 399 fits
			// a uint32_t with room to spare.
			t->alpha = (uint8_t)(255u - (255u * gone) / AIMTEXT_FADE_MS);
		}
	}

	return t->alpha != 0;
}

const char* aimTextLabel(const AimText* t)
{
	return t->alpha != 0 ? t->text : NULL;
}

uint8_t aimTextAlpha(const AimText* t)
{
	return t->alpha;
}
