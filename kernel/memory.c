#include "../includes/std.h"

void* memcpy(void* dest, const void* src, size_t size) {
    uint8_t* out = dest;
    const uint8_t* in = src;
    size_t i;
    for (i = 0; i < size; i++) out[i] = in[i];
    return dest;
}

void* memset(void* dest, int value, size_t size) {
    uint8_t* out = dest;
    size_t i;
    for (i = 0; i < size; i++) out[i] = (uint8_t)value;
    return dest;
}
