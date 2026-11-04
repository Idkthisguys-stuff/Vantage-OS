/* SDL2 platform backend for the Vantage-OS simulator.
 *
 * Implements the seam declared in includes/platform.h. Built only by
 * sim/CMakeLists.txt; this file is never compiled into kernel8.img.
 */

#include "../../includes/platform.h"
#include "../../includes/io.h"

#include "SDL2/SDL.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

/* Without this the process is DPI-unaware, so Windows bitmap-stretches the
 * entire window by the system scale factor: a 480x800 canvas becomes 720x1200
 * on a 150% display and overflows the screen. Marking the process per-monitor
 * aware and pairing it with SDL_WINDOW_ALLOW_HIGHDPI makes the window that many
 * real pixels regardless of the desktop scale setting. */
static void claim_dpi_awareness(void) {
#if defined(_WIN32)
    /* Windows 10 1703+. Falls back below if unavailable. */
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        return;
    }
    SetProcessDPIAware();
#endif
}

static uint32_t* backBuffer;
static uint32_t canvasW;
static uint32_t canvasH;

static SDL_Window* window;
static SDL_Renderer* renderer;
static SDL_Texture* texture;
static uint32_t lastFrame;

static int mouseDown;
static int mouseX;
static int mouseY;

static const uint8_t* coordinate_glyph(char c) {
    static const uint8_t digits[10][5] = {
        { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 },
        { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 },
        { 5, 5, 7, 1, 1 }, { 7, 4, 7, 1, 7 },
        { 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 },
        { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 }
    };
    static const uint8_t x[5] = { 5, 5, 2, 5, 5 };
    static const uint8_t y[5] = { 5, 5, 2, 2, 2 };
    static const uint8_t colon[5] = { 0, 2, 0, 2, 0 };
    static const uint8_t minus[5] = { 0, 0, 7, 0, 0 };
    static const uint8_t blank[5] = { 0, 0, 0, 0, 0 };

    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c == 'X') return x;
    if (c == 'Y') return y;
    if (c == ':') return colon;
    if (c == '-') return minus;
    return blank;
}

static void draw_coordinates(void) {
    char text[64];
    SDL_Rect background;
    size_t i;
    int row, col;

    snprintf(text, sizeof(text), "X:%d Y:%d", mouseX, mouseY);
    background.x = 6;
    background.y = 6;
    background.w = (int)strlen(text) * 12 + 12;
    background.h = 27;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderFillRect(renderer, &background);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    for (i = 0; text[i] != '\0'; i++) {
        const uint8_t* glyph = coordinate_glyph(text[i]);
        for (row = 0; row < 5; row++) {
            for (col = 0; col < 3; col++) {
                if (glyph[row] & (1 << (2 - col))) {
                    SDL_Rect pixel = { 12 + (int)i * 12 + col * 3,
                                       12 + row * 3, 3, 3 };
                    SDL_RenderFillRect(renderer, &pixel);
                }
            }
        }
    }
}

void uart_writeText(const char* buffer) {
    SDL_Log("%s", buffer);
}

uint32_t* fb_init(uint32_t width, uint32_t height) {
    canvasW = height;
    canvasH = width;

    claim_dpi_awareness();

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 0;
    }

    window = SDL_CreateWindow("Vantage-OS", SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED, (int)canvasW,
                              (int)canvasH,
                              SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return 0;
    }

    renderer = SDL_CreateRenderer(window, -1,
                                  SDL_RENDERER_ACCELERATED |
                                      SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        return 0;
    }

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888,
                                SDL_TEXTUREACCESS_STREAMING, (int)canvasW,
                                (int)canvasH);
    if (!texture) {
        SDL_Log("SDL_CreateTexture failed: %s", SDL_GetError());
        return 0;
    }

    backBuffer = (uint32_t*)calloc((size_t)canvasW * canvasH, sizeof(uint32_t));
    lastFrame = time_us();
    return backBuffer;
}

void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= canvasW || y >= canvasH) {
        return;
    }
    backBuffer[y * canvasW + x] = color;
}

void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    uint32_t i;
    uint32_t j;

    if (x >= canvasW || y >= canvasH) {
        return;
    }
    if (x + w > canvasW) {
        w = canvasW - x;
    }
    if (y + h > canvasH) {
        h = canvasH - y;
    }

    for (j = 0; j < h; j++) {
        uint32_t* row = backBuffer + (size_t)(y + j) * canvasW + x;
        for (i = 0; i < w; i++) {
            row[i] = color;
        }
    }
}

static uint32_t poll_touch_event(TouchEvent* result) {
    SDL_Event e;

    result->type = TOUCH_EVENT_NONE;
    result->x = mouseX;
    result->y = mouseY;
    SDL_PumpEvents();
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) > 0) {
        switch (e.type) {
            case SDL_QUIT:
                SDL_Quit();
                exit(0);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT && !mouseDown) {
                    mouseDown = 1;
                    mouseX = e.button.x;
                    mouseY = e.button.y;
                    result->type = TOUCH_EVENT_DOWN;
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT && mouseDown) {
                    mouseDown = 0;
                    mouseX = e.button.x;
                    mouseY = e.button.y;
                    result->type = TOUCH_EVENT_UP;
                }
                break;
            case SDL_MOUSEMOTION:
                if (mouseDown && (mouseX != e.motion.x || mouseY != e.motion.y)) {
                    mouseX = e.motion.x;
                    mouseY = e.motion.y;
                    result->type = TOUCH_EVENT_MOVE;
                }
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_CLOSE) {
                    SDL_Quit();
                    exit(0);
                } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && mouseDown) {
                    mouseDown = 0;
                    result->type = TOUCH_EVENT_UP;
                }
                break;
            default:
                break;
        }
        if (result->type != TOUCH_EVENT_NONE) {
            result->x = mouseX;
            result->y = mouseY;
            return 1;
        }
    }
    return 0;
}

void fb_wait_frame(void) {
    uint32_t elapsed = time_us() - lastFrame;

    if (elapsed < 16667u) {
        SDL_Delay((16667u - elapsed + 999u) / 1000u);
    }
    lastFrame = time_us();
}

void fb_swap(void) {
    if (!texture) {
        return;
    }
    SDL_UpdateTexture(texture, NULL, backBuffer,
                      (int)(canvasW * sizeof(uint32_t)));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    draw_coordinates();
    SDL_RenderPresent(renderer);
}

uint32_t time_us(void) {
    static Uint64 freq = 0;

    if (freq == 0) {
        freq = SDL_GetPerformanceFrequency();
        if (freq == 0) {
            return 0;
        }
    }
    return (uint32_t)((SDL_GetPerformanceCounter() * 1000000ULL) / freq);
}

uint32_t touch_init(void) {
    mouseDown = 0;
    mouseX = 0;
    mouseY = 0;
    return 1;
}

uint32_t touch_read(TouchEvent* event) {
    return poll_touch_event(event);
}
