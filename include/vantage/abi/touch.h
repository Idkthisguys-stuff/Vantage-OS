#pragma once
#include <vantage/base.h>

typedef enum {
    TOUCH_EVENT_NONE = 0,
    TOUCH_EVENT_DOWN,
    TOUCH_EVENT_MOVE,
    TOUCH_EVENT_UP
} TouchEventType;

typedef struct {
    TouchEventType type;
    uint32_t slot;
    uint8_t id;
    int x;
    int y;
} TouchEvent;
