#include "../../includes/env/windowManager.h"
#include "../../user/include/user_lib.h"
#include <SDL2/SDL.h>

static int check_pixel(SDL_Renderer* renderer, int x, int y, uint32_t expected) {
    SDL_Rect rect = { x, y, 1, 1 };
    uint32_t pixel = 0;

    if (SDL_RenderReadPixels(renderer, &rect, SDL_PIXELFORMAT_XRGB8888,
                             &pixel, sizeof(pixel)) != 0) {
        SDL_Log("Readback failed: %s", SDL_GetError());
        return 0;
    }
    if ((pixel & 0x00ffffffu) != expected) {
        SDL_Log("Unexpected pixel at (%d, %d): %08x", x, y, pixel);
        return 0;
    }
    return 1;
}

int main(int argc, char* argv[]) {
    SDL_Event event;
    SDL_Window* window;
    SDL_Renderer* renderer;
    int touch[2] = { -1, -1 };
    int width, height;
    (void)argc;
    (void)argv;

    env_Init(480, 800);
    window = SDL_GetWindowFromID(1);
    renderer = window ? SDL_GetRenderer(window) : NULL;
    if (!renderer) {
        SDL_Log("Simulator initialization failed: %s", SDL_GetError());
        return 1;
    }
    SDL_GetWindowSize(window, &width, &height);
    if (width != 480 || height != 800 || sys_touch_read(touch) != 0) {
        return 1;
    }

    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = 120;
    event.button.y = 240;
    if (SDL_PushEvent(&event) != 1 || sys_touch_read(touch) != 1 ||
        touch[0] != 120 || touch[1] != 240) {
        SDL_Log("Mouse press did not reach the syscall dispatcher");
        return 1;
    }

    sys_canvas_set_color(RGB(10, 10, 30));
    sys_canvas_clear();
    sys_canvas_set_color(RGB(255, 255, 255));
    sys_canvas_fill_circle(touch[0], touch[1], 12);
    sys_canvas_present();
    if (!check_pixel(renderer, 0, 0, RGB(10, 10, 30)) ||
        !check_pixel(renderer, 120, 240, RGB(255, 255, 255)) ||
        !check_pixel(renderer, 140, 240, RGB(10, 10, 30))) {
        return 1;
    }

    SDL_zero(event);
    event.type = SDL_MOUSEMOTION;
    event.motion.x = 200;
    event.motion.y = 300;
    if (SDL_PushEvent(&event) != 1 || sys_touch_read(touch) != 1 ||
        touch[0] != 200 || touch[1] != 300) {
        return 1;
    }

    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONUP;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = 220;
    event.button.y = 320;
    if (SDL_PushEvent(&event) != 1 || sys_touch_read(touch) != 0 ||
        touch[0] != 220 || touch[1] != 320) {
        return 1;
    }
    sys_canvas_set_color(RGB(10, 10, 30));
    sys_canvas_clear();
    sys_canvas_present();
    if (!check_pixel(renderer, 120, 240, RGB(10, 10, 30))) {
        return 1;
    }

    SDL_Quit();
    return 0;
}
