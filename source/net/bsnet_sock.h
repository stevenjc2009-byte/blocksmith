/* bsnet_sock.h — the ONLY platform seam in the transport.
 *
 * Everything else in bsnet_transport.c talks plain POSIX sockets: socket(),
 * sendto(), recvfrom(), fcntl(..., O_NONBLOCK), gethostbyname(), close(). Both
 * libctru's SOC service (3DS) and glibc (the host test build) implement that
 * same surface under the same names, so no wrapping is needed there — one
 * copy of the protocol logic compiles for both targets unmodified.
 *
 * The two things that are NOT the same on both platforms are isolated here:
 *   - bringing the socket service up/down (SOCU needs an aligned buffer and
 *     socInit()/socExit(); the host has nothing to do)
 *   - a monotonic millisecond clock (3DS homebrew commonly uses osGetTime()
 *     rather than assuming clock_gettime(CLOCK_MONOTONIC) is wired up)
 */

#ifndef BSNET_SOCK_H
#define BSNET_SOCK_H

#include <stdbool.h>
#include <stdint.h>

/* Starts the platform socket service. Safe to call more than once (idempotent
 * while already up). False on failure — netTransportInit() surfaces that as a
 * player-facing error rather than crashing later on the first socket() call. */
bool bsSockPlatformInit(void);

/* Tears the socket service down. Safe to call even if never (successfully)
 * started. */
void bsSockPlatformExit(void);

/* Monotonic milliseconds since an arbitrary epoch. Never goes backwards;
 * every timer and retry deadline in the transport is expressed in this clock
 * so the same arithmetic works on both platforms. */
uint64_t bsSockNowMs(void);

#endif /* BSNET_SOCK_H */
