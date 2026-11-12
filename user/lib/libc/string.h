#pragma once
/* Freestanding C library subset for third-party code in EL0 programs
 * (BearSSL); implemented by lib/memory.c. Not used by host builds. */
#include <vantage/base.h>

void* memmove(void* dest, const void* src, size_t n);
size_t strlen(const char* text);
