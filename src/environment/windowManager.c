#include "../../includes/environment/windowManager.h"

#define HEADER_ENABLE false

void env_Init(uint32_t width, uint32_t height) {
    canvas_init(width, height);
    uint32_t touchStatus = touch_init();
}

void env_DrawWindow(Window* window) {
    #if HEADER_ENABLE
        canvas_set_color(window->titleColor);
        canvas_fill_rect(window->x, window->y, window->w, 20);
        canvas_set_color(window->bgColor);
        canvas_fill_rect(window->x + 1, window->y + 20, window->w - 1, window->h - 20);
    #else
        canvas_set_color(window->bgColor);
        canvas_fill_rect(window->x, window->y, window->w, window->h);
    #endif
}

void env_ListenTouch(Window* window, int tx, int ty, uint32_t isTouching) {
    if (!isTouching) {
        window->dragState = 0;
        return;
    }

    if (tx >= window->x && tx <= (window->x + window->w) &&
        ty >= window->y && ty <= (window->y + window->h)) {
        if (ty <= window->y + 20) {
            if (!window->dragState) {
                window->dragState = 1;
                window->dragOffsetX = tx - window->x;
                window->dragOffsetY = ty - window->y;
            } else {
                window->x = tx - window->dragOffsetX;
                window->y = ty - window->dragOffsetY;
            }
        } else {
            window->dragState = 0;
            if (window->onTouch) {
                window->onTouch(window, tx, ty);
            }
        }
    } else {
        window->dragState = 0;
    }
}