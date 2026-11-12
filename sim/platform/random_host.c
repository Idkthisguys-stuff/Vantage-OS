/* Desktop entropy: splitmix64 seeded from the host clock for network
 * identifiers, and the operating system's generator for net_entropy.
 * Simulator only. */
#include <vantage/kernel/net.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <stdio.h>
#endif

uint32_t net_port_random(void) {
    static uint64_t state;
    if (!state) state = (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)&state ^ ((uint64_t)clock() << 32);
    uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return (uint32_t)((z ^ (z >> 31)) >> 32);
}

int net_entropy(void* data, unsigned int bytes) {
#ifdef _WIN32
    return BCryptGenRandom(NULL, data, bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : NET_UNAVAILABLE;
#else
    FILE* source = fopen("/dev/urandom", "rb");
    if (!source) return NET_UNAVAILABLE;
    size_t got = fread(data, 1, bytes, source);
    fclose(source);
    return got == bytes ? 0 : NET_UNAVAILABLE;
#endif
}
