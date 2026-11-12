#pragma once
#include "terminal.h"
#include <vantage/user.h>

#define NET_STATE_FITS(type) _Static_assert(sizeof(type) <= TERMINAL_JOB_STATE, #type " exceeds job state")

void net_error(Terminal* t, int error);
void net_fail(Terminal* t, int error);
void net_address(Terminal* t, uint32_t address);
void net_ssid(Terminal* t, const char* ssid, uint32_t length);
uint32_t net_elapsed(uint32_t since);
int net_equals(const char* a, const char* b);
/* Next argument into out; double quotes allow spaces. NULL if malformed. */
const char* net_argument(const char* text, char* out, uint32_t capacity, uint32_t* length);
int net_rest_empty(const char* text);
/* 1 resolved, 0 pending, -1 failed (reported). */
int net_resolve_step(Terminal* t, const char* host, uint32_t* address);

void net_ping(Terminal* t, const char* args);
void net_lookup(Terminal* t, const char* args);
void net_get(Terminal* t, const char* args);

/* Saved networks file: 0 or a FileError. remove returns 1 when found. */
int net_profiles_store(const char* ssid, uint32_t ssid_length, const char* passphrase, uint32_t flags);
int net_profiles_remove(const char* ssid, uint32_t ssid_length);
