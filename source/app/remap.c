#include "app/remap.h"

#include <string.h>

// Human-readable names for each action, same order as OptionsAction.
static const char* const s_action_names[ACTION_COUNT] = {
	"Move forward",
	"Move back",
	"Move left",
	"Move right",
	"Jump",
	"Break",
	"Place",
};

// Human-readable names for each valid key bit, same order as OPTIONS_VALID_KEYS.
static const char* const s_key_names[OPTIONS_VALID_KEY_COUNT] = {
	"A", "X", "Y", "D-right", "D-left", "D-up", "D-down",
};

// The default binding for each action, same order as s_action_defaults in
// options.c. Duplicated here rather than shared because options.c's copy is
// static and this module must not reach into it.
static const uint32_t s_defaults[ACTION_COUNT] = {
	OPT_KEY_DUP, OPT_KEY_DDOWN, OPT_KEY_DLEFT, OPT_KEY_DRIGHT,
	OPT_KEY_A, OPT_KEY_X, OPT_KEY_Y,
};

void remapInit(RemapState* rs, const Options* o)
{
	if (!rs) return;
	if (o) {
		memcpy(rs->bindings, o->bindings, sizeof(rs->bindings));
	} else {
		Options def;
		optionsDefaults(&def);
		memcpy(rs->bindings, def.bindings, sizeof(rs->bindings));
	}
	rs->capture_action = ACTION_COUNT;
}

void remapApply(const RemapState* rs, Options* o)
{
	if (!rs || !o) return;
	memcpy(o->bindings, rs->bindings, sizeof(o->bindings));
}

int remapActionCount(void) { return ACTION_COUNT; }

const char* remapActionName(int action)
{
	if (action < 0 || action >= ACTION_COUNT) return NULL;
	return s_action_names[action];
}

const char* remapKeyName(uint32_t key)
{
	for (int i = 0; i < OPTIONS_VALID_KEY_COUNT; i++)
		if (OPTIONS_VALID_KEYS[i] == key) return s_key_names[i];
	return "???";
}

uint32_t remapGetBinding(const RemapState* rs, int action)
{
	if (!rs || action < 0 || action >= ACTION_COUNT) return 0;
	return rs->bindings[action];
}

void remapStartCapture(RemapState* rs, int action)
{
	if (!rs || action < 0 || action >= ACTION_COUNT) return;
	rs->capture_action = action;
}

bool remapIsCapturing(const RemapState* rs)
{
	return rs && rs->capture_action < ACTION_COUNT;
}

int remapCaptureAction(const RemapState* rs)
{
	if (!rs) return ACTION_COUNT;
	return rs->capture_action;
}

int remapFindAction(const RemapState* rs, uint32_t key)
{
	if (!rs || key == 0) return ACTION_COUNT;
	for (int i = 0; i < ACTION_COUNT; i++)
		if (rs->bindings[i] == key) return i;
	return ACTION_COUNT;
}

bool remapApplyCapture(RemapState* rs, uint32_t key)
{
	if (!rs || rs->capture_action >= ACTION_COUNT) return false;

	const int target = rs->capture_action;
	rs->capture_action = ACTION_COUNT;

	if (key == 0) return false;

	const int existing = remapFindAction(rs, key);

	if (existing == target) return false;

	if (existing >= 0 && existing < ACTION_COUNT) {
		// Swap: the existing action gets the old binding, this action gets the key.
		const uint32_t old = rs->bindings[target];
		rs->bindings[existing] = old;
		rs->bindings[target] = key;
		return true;
	}

	// Key was unbound — just assign it.
	rs->bindings[target] = key;
	return false;
}

void remapReset(RemapState* rs)
{
	if (!rs) return;
	memcpy(rs->bindings, s_defaults, sizeof(rs->bindings));
}
