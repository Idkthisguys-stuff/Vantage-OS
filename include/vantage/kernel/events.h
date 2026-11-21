#pragma once
#include <vantage/abi/event.h>
#include <vantage/abi/key.h>
#include <vantage/abi/touch.h>

#define EVENT_SERVICE_US 8000U

/* Core-0-only input and driver service, shared by the direct reads and wait. */
uint32_t event_read_key(KeyEvent* key);
uint32_t event_read_touch(TouchContact* contact);
int event_wait(uint32_t interests, uint32_t timeout_us);
void event_service(void);
