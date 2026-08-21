// Step 6 — Dedicated map equipment slot & live minimap.
//
// Pure C logic (fog bit ops, color lookup, save/load) compilable on host for testing.
// 3DS rendering (texture upload, sprite drawing) is behind #ifndef __3DS__ guards so
// the host suite can link the full module without <citro3d.h>.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/block.h"
#include "world/world.h"

#define MINIMAP_SIZE      64
#define MINIMAP_EXPLORED  8
#define MINIMAP_FOG_BYTES (MINIMAP_SIZE * MINIMAP_SIZE / 8)
#define MINIMAP_FOG_WORDS (MINIMAP_SIZE * MINIMAP_SIZE / 64)

// Save format magic and version. Stored as little-endian in the file header.
#define MINIMAP_SAVE_MAGIC  0x4D4D4647u  // "MMFG"
#define MINIMAP_SAVE_VER    1

// Fog bitmap — 1 bit per minimap cell, 512 bytes total.
typedef struct {
	uint64_t bits[MINIMAP_FOG_WORDS];
} MinimapFog;

// ── Pure C — host testable ─────────────────────────────────────────────────────────────

// Returns the display colour for a block id. Air returns black. Unknown ids return magenta.
uint32_t minimapBlockColor(BlockId id);

// Fog bit operations. All index arguments are clamped; no UB on out-of-range.
void minimapFogClear(MinimapFog* f);
bool minimapFogGet(const MinimapFog* f, int idx);
void minimapFogSet(MinimapFog* f, int idx);

// Marks every cell within MINIMAP_EXPLORED blocks of (playerX, playerZ) as explored.
// `offsetX/Z` is the world-coordinate of minimap cell (0,0).
void minimapFogUpdate(MinimapFog* f, int playerX, int playerZ, int offsetX, int offsetZ);

// Crash-safe write (.tmp → fclose → remove → rename). False on IO failure.
bool minimapSave(const MinimapFog* f, const char* path);

// Loads fog from `path`. On wrong magic/version, fills `f` with zeros and returns false.
// Missing file fills `f` with zeros and returns true (not an error, same as optionsLoad).
bool minimapLoad(MinimapFog* f, const char* path);

// Visibility. The map stays hidden until the caller equips/toggles it; this module only
// holds the flag so every caller agrees on one state.
void minimapSetVisible(bool v);
bool minimapIsVisible(void);

// ── 3DS rendering — console build only ──────────────────────────────────────────────────
#ifdef __3DS__

// Allocates the minimap texture. Returns false if allocation fails.
bool minimapGfxInit(void);

// Frees the minimap texture.
void minimapGfxExit(void);

// Draws the minimap (64x64 px, 1 px/block) plus a thin border and player arrow.
// `yaw` is the player's camera yaw in radians. Must be called between
// spriteBegin(320,240) and spriteEnd().
void minimapDraw(const MinimapFog* f, int centerX, int centerZ, int offsetX, int offsetZ,
                 float yaw);

#endif
