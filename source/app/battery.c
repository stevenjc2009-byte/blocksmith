#include "app/battery.h"

#include <3ds.h>
#include <time.h>

#include "gfx/sprite.h"

#define POLL_INTERVAL_MS 1000

static int  s_level;
static bool s_charging;
static u64  s_last_poll_tick;

static u64 tickMs(void)
{
	return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000);
}

void batteryInit(void)
{
	ptmuInit();
	s_level         = 0;
	s_charging      = false;
	s_last_poll_tick = 0;
}

void batteryExit(void)
{
	ptmuExit();
}

void batteryPoll(void)
{
	const u64 now = tickMs();
	if (now - s_last_poll_tick < POLL_INTERVAL_MS) return;
	s_last_poll_tick = now;

	u8 level = 0;
	u8 charge = 0;
	PTMU_GetBatteryLevel(&level);
	PTMU_GetBatteryChargeState(&charge);

	s_level    = (int)level;
	s_charging = charge != 0;
}

int batteryLevel(void) { return s_level; }
bool batteryCharging(void) { return s_charging; }

int batteryBars(void)
{
	// Level 0-5 maps to 0-4 bars. Level 0 = 0 bars, 1 = 1, 2 = 2, 3-4 = 3, 5 = 4.
	if (s_level <= 0) return 0;
	if (s_level == 1) return 1;
	if (s_level == 2) return 2;
	if (s_level <= 4) return 3;
	return 4;
}

bool batteryLow(void)
{
	return s_level <= 1 && !s_charging;
}

void batteryDraw(float x, float y)
{
	const float bar_w  = 6.0f;
	const float bar_h  = 10.0f;
	const float gap    = 1.0f;
	const float outline = 1.0f;
	const int bars = batteryBars();

	const uint32_t outline_col = SPRITE_RGBA(200, 200, 200, 255);
	const uint32_t fill_col    = batteryLow()
		? SPRITE_RGBA(220, 50, 50, 255)
		: SPRITE_RGBA(80, 200, 80, 255);
	const uint32_t empty_col   = SPRITE_RGBA(60, 60, 60, 128);

	// Outline
	spriteRect(x, y, bar_w * 4 + gap * 3 + outline * 2,
	           bar_h + outline * 2, outline_col);

	// Background inside outline
	spriteRect(x + outline, y + outline,
	           bar_w * 4 + gap * 3, bar_h, empty_col);

	// Filled bars
	for (int i = 0; i < bars; i++) {
		float bx = x + outline + (float)i * (bar_w + gap);
		spriteRect(bx, y + outline, bar_w, bar_h, fill_col);
	}
}
