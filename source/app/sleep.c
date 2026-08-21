#include "app/sleep.h"

#include <3ds.h>

#include "net/bsnet.h"

static aptHookCookie s_hook;
static bool s_asleep;

static void onAptHook(APT_HookType hook, void* param)
{
	(void)param;
	switch (hook) {
	case APTHOOK_ONSLEEP:
		s_asleep = true;
		osSetSpeedupEnable(false);
		sleepNetHeartbeat();
		break;
	case APTHOOK_ONWAKEUP:
		osSetSpeedupEnable(true);
		s_asleep = false;
		break;
	default:
		break;
	}
}

void sleepInit(void)
{
	s_asleep = false;
	aptHook(&s_hook, onAptHook, NULL);
}

void sleepExit(void)
{
	aptUnhook(&s_hook);
	s_asleep = false;
}

bool sleepIsAsleep(void) { return s_asleep; }

bool sleepShouldSkip(void) { return s_asleep; }

void sleepNetHeartbeat(void)
{
	// No-op safe: netUpdate() already handles keepalives. During sleep the
	// socket stays alive via OS-level UDP; on wake netUpdate() resumes
	// pumping normally. If a future transport change needs an explicit
	// ping here, netHeartbeat() is the place.
}
