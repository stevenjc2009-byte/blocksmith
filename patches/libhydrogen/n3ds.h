// Nintendo 3DS backend.
//
// Entropy comes from PS_GenerateRandomBytes() (libctru's <3ds/services/ps.h>),
// which is documented as generating "cryptographically secure random bytes"
// via the console's PS (Process Services) system module — the same hardware-
// backed CSPRNG the OS itself relies on. There is no /dev/urandom, no
// RtlGenRandom, and no getrandom() on this bare-metal target, so none of the
// existing backends in this directory apply; this mirrors the shape of
// impl/random/windows.h (the other backend that reads a single secure block
// straight from a platform RNG call, rather than hashing several small draws
// the way impl/random/esp32.h and impl/random/nrf52832.h do).
//
// The caller (application startup, not this file) must have already called
// psInit() — same requirement every libctru service has, and every other
// backend in this directory assumes its platform's entropy source is already
// available rather than initializing it here.
#include <3ds.h>

static int
hydro_random_init(void)
{
    if (R_FAILED(PS_GenerateRandomBytes(hydro_random_context.state,
                                         sizeof hydro_random_context.state))) {
        return -1;
    }
    hydro_random_context.counter = ~LOAD64_LE(hydro_random_context.state);

    return 0;
}
