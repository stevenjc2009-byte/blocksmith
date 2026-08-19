/* bsnet_sock.c — SOCU vs. host, see bsnet_sock.h for why this is the only
 * platform-split file in the transport. */

/* Must be defined before ANY header is pulled in — including bsnet_sock.h's
 * own <stdbool.h>/<stdint.h> — because glibc's feature-test macros latch in
 * on the first system header, not on the first one that happens to need
 * them. Without this, clock_gettime()/CLOCK_MONOTONIC are invisible under
 * strict -std=c11 in the host build below. Harmless on the 3DS branch. */
#ifndef __3DS__
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif
#endif

#include "bsnet_sock.h"

#ifdef __3DS__

#include <3ds.h>
#include <malloc.h>

/* Lifted from the standard devkitPro 3ds-examples sockets sample: SOCU wants
 * a page-aligned buffer, and 128 KiB is comfortably more than this transport
 * ever has in flight (one UDP datagram at a time, <= BS_MAX_PACKET bytes). */
#define BS_SOC_ALIGN       0x1000
#define BS_SOC_BUFFERSIZE  (128 * 1024)

static u32 *s_soc_buffer = NULL;

bool bsSockPlatformInit(void)
{
    if (s_soc_buffer != NULL) return true; /* already up */

    s_soc_buffer = (u32 *)memalign(BS_SOC_ALIGN, BS_SOC_BUFFERSIZE);
    if (s_soc_buffer == NULL) return false;

    Result rc = socInit(s_soc_buffer, BS_SOC_BUFFERSIZE);
    if (R_FAILED(rc)) {
        free(s_soc_buffer);
        s_soc_buffer = NULL;
        return false;
    }
    return true;
}

void bsSockPlatformExit(void)
{
    if (s_soc_buffer == NULL) return;
    socExit();
    free(s_soc_buffer);
    s_soc_buffer = NULL;
}

uint64_t bsSockNowMs(void)
{
    /* Milliseconds since 1900, per ctrulib's os.h. Only relative differences
     * are ever used, so the epoch does not matter. */
    return (uint64_t)osGetTime();
}

#else /* host build (bsnet_transport_hosttest) */

#include <stddef.h>
#include <time.h>

bool bsSockPlatformInit(void)
{
    return true; /* the host has no socket service to bring up */
}

void bsSockPlatformExit(void)
{
}

uint64_t bsSockNowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

#endif
