#pragma once
#include <vantage/base.h>

#define SYS_EVENT_WAIT 14U
#define EVENT_WAIT_MAX_US 1000000U

/* Input readiness lasts until the corresponding read call consumes it.
 * Network and Bluetooth events indicate a change. Query the current status
 * after waking. Multiple changes may coalesce into one event. */
enum {
    EVENT_KEY = 1U, EVENT_TOUCH = 2U, EVENT_NETWORK = 4U, EVENT_BLUETOOTH = 8U, EVENT_USB = 16U,
    EVENT_ALL = EVENT_KEY | EVENT_TOUCH | EVENT_NETWORK | EVENT_BLUETOOTH | EVENT_USB
};
