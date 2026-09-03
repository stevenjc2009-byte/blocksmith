// Host stub for tests/metrics_timing_test.c: the citro3d surface source/debug/metrics.c and
// source/gfx/screen.h (included transitively) name. Nothing here is measured by the suite --
// the three getters below feed metrics.c's cpu_ms/gpu_ms/cmdbuf columns, which the suite does
// not assert on. The figures the suite DOES assert on, frame_ms and sync_ms, come from
// svcGetSystemTick (tests/metrics_timing_stub/3ds.h), which the test file owns.
//
// Original shape: METRICS-VISIBLE's scratchpad harness (metricsvis_stub/citro3d.h).
#pragma once

#include <3ds.h>

typedef struct { int dummy; } C3D_RenderTarget;
typedef struct { int dummy; } C3D_Tex;

float C3D_GetProcessingTime(void);
float C3D_GetDrawingTime(void);
float C3D_GetCmdBufUsage(void);
