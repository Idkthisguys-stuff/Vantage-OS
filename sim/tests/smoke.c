#include <vantage/kernel/window_manager.h>
#include <vantage/user.h>
#include <vantage/kernel/usb_media.h>
#include <SDL2/SDL.h>
#include "../platform/display_sim.h"
#include "../platform/net_sim.h"

void home_app_init(void);
void home_app_step(void);

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

static int push_click(int x, int y) {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = x;
    event.button.y = y;
    if (SDL_PushEvent(&event) != 1) return 0;
    event.type = SDL_MOUSEBUTTONUP;
    return SDL_PushEvent(&event) == 1;
}

enum { NETWORK_ROW_WIDTH = 28 * TEXT_CELL_WIDTH };

static int read_ethernet_row(SDL_Renderer* renderer, uint32_t* pixels) {
    SDL_Rect rect = { 3 * TEXT_CELL_WIDTH, 12 * TEXT_CELL_HEIGHT,
                      NETWORK_ROW_WIDTH, TEXT_CELL_HEIGHT };
    return SDL_RenderReadPixels(renderer, &rect, SDL_PIXELFORMAT_XRGB8888,
                                pixels, NETWORK_ROW_WIDTH * sizeof(*pixels)) == 0;
}

static int test_network_refresh(SDL_Renderer* renderer) {
    uint32_t unplugged[NETWORK_ROW_WIDTH * TEXT_CELL_HEIGHT];
    uint32_t current[NETWORK_ROW_WIDTH * TEXT_CELL_HEIGHT];
    if (!push_click(295, 66)) return 0;
    home_app_step();
    if (!read_ethernet_row(renderer, unplugged)) return 0;

    for (int cable = 1; cable >= 0; cable--) {
        sim_net_set_cable(cable);
        uint32_t started = SDL_GetTicks();
        do {
            home_app_step();
            if (!read_ethernet_row(renderer, current)) return 0;
            if ((memcmp(current, unplugged, sizeof(current)) != 0) == cable) break;
            if (SDL_GetTicks() - started >= 2000) {
                SDL_Log("Network view did not update for cable state %d", cable);
                return 0;
            }
            SDL_Delay(10);
        } while (1);
    }
    if (!push_click(40, 50)) return 0;
    home_app_step();
    return 1;
}

static void put32(uint8_t* data, uint32_t value) {
    for (unsigned int i = 0; i < 4; i++) data[i] = (uint8_t)(value >> (i * 8));
}

static int usb_sector(void* context, uint32_t lba, void* output) {
    (void)context;
    uint8_t* data = output;
    memset(data, 0, USB_FILE_CHUNK);
    if (!lba) {
        data[12] = 2; data[13] = 1; data[14] = 32; data[16] = 1;
        put32(data + 32, 66100); put32(data + 36, 513); put32(data + 44, 2);
        data[510] = 0x55; data[511] = 0xaa;
    } else if (lba == 32) {
        put32(data + 8, 0x0fffffff); put32(data + 12, 4); put32(data + 16, 0x0fffffff);
    } else if (lba == 545) {
        memcpy(data, "HELLO   TXT", 11); data[26] = 3; put32(data + 28, 515);
    } else if (lba == 546) {
        memset(data, 'A', USB_FILE_CHUNK);
    } else if (lba == 547) {
        memcpy(data, "END", 3);
    } else return HW_IO;
    return HW_OK;
}

static int click_and_step(int x, int y) {
    if (!push_click(x, y)) return 0;
    home_app_step();
    return 1;
}

static int test_usb_files(SDL_Renderer* renderer) {
    UsbFat32Source source = { NULL, 66100, usb_sector };
    if (usb_media_attach(&source) != HW_OK || !(sys_event_wait(EVENT_USB, 0) & EVENT_USB))
        return 0;
    UsbFileCall status = { .operation = USB_FILES_STATUS };
    if (sys_usb_files(&status) || status.count != 1 ||
        !click_and_step(185, 66) || !click_and_step(400, 51) ||
        !click_and_step(70, 200) || !check_pixel(renderer, 340, 720, RGB(30, 61, 67)))
        return 0;

    /* The first chunk is replayable after the UI reads it. The second comes
     * through the Next action and remount invalidates the original generation. */
    UsbFileCall read = { .operation = USB_FILES_READ, .generation = status.generation };
    if (sys_usb_files(&read) || read.length != 512 || read.data[511] != 'A' ||
        !click_and_step(350, 720)) return 0;
    read.offset = 512;
    if (sys_usb_files(&read) || read.length != 3 || memcmp(read.data, "END", 3) ||
        !check_pixel(renderer, 340, 720, RGB(13, 24, 29))) return 0;
    usb_media_detach();
    home_app_step();
    if (sys_usb_files(&read) != FILE_UNAVAILABLE ||
        !click_and_step(40, 51) || !click_and_step(40, 51)) return 0;
    return 1;
}

static int test_wifi_and_clock(SDL_Renderer* renderer) {
    SDL_Rect clock = {24, 7 * TEXT_CELL_HEIGHT, 272, TEXT_CELL_HEIGHT};
    uint32_t before[272 * TEXT_CELL_HEIGHT], after[272 * TEXT_CELL_HEIGHT];
    if (SDL_RenderReadPixels(renderer, &clock, SDL_PIXELFORMAT_XRGB8888, before, 272 * 4)) return 0;
    if (!click_and_step(290, 65) || !click_and_step(350, 310)) return 0;
    uint32_t started = SDL_GetTicks();
    while (SDL_GetTicks() - started < 850) {
        if (sys_event_wait(EVENT_NETWORK, 50 * 1000) < 0) return 0;
        home_app_step();
    }
    /* The strongest virtual network is the WPA2 demo AP. */
    if (!click_and_step(65, 360)) return 0;
    SDL_Event event;
    SDL_zero(event); event.type = SDL_TEXTINPUT;
    memcpy(event.text.text, "vantage", sizeof("vantage"));
    if (SDL_PushEvent(&event) != 1) return 0;
    home_app_step();
    /* Use the touch symbol page to type the hyphen. */
    if (!click_and_step(100, 760) || !click_and_step(125, 610)) return 0;
    memcpy(event.text.text, "demo", sizeof("demo"));
    if (SDL_PushEvent(&event) != 1) return 0;
    home_app_step();
    if (!click_and_step(380, 51)) return 0;
    started = SDL_GetTicks();
    NetTime time = {0};
    while (sys_net_time(&time) <= 0 && SDL_GetTicks() - started < 4000) {
        if (sys_event_wait(EVENT_NETWORK, 100000) < 0) return 0;
        home_app_step();
    }
    NetStatus status;
    if (!time.synced || sys_net_status(&status) || status.link != NET_LINK_UP ||
        status.configuration != NET_ADDRESS_BOUND || !click_and_step(40, 51)) return 0;
    home_app_step();
    return !SDL_RenderReadPixels(renderer, &clock, SDL_PIXELFORMAT_XRGB8888, after, 272 * 4) &&
           memcmp(before, after, sizeof(before)) != 0;
}

int main(int argc, char* argv[]) {
    SDL_Event event;
    SDL_Window* window;
    SDL_Renderer* renderer;
    TouchEvent touch;
    int width, height;
    Window test_window = { 0 };
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
    if (width != 480 || height != 800 || sys_touch_read(&touch) != 0 ||
        touch.type != TOUCH_EVENT_NONE) {
        return 1;
    }
    if (sys_event_wait(EVENT_ALL + 1, 0) != -1 ||
        sys_event_wait(0, EVENT_WAIT_MAX_US + 1) != -1 ||
        sys_event_wait(EVENT_TOUCH, 0) != 0 || sys_event_wait(0, 2000) != 0) return 1;

    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = 120;
    event.button.y = 240;
    if (SDL_PushEvent(&event) != 1 || sys_event_wait(EVENT_TOUCH, 0) != EVENT_TOUCH ||
        sys_event_wait(EVENT_TOUCH, 8000) != EVENT_TOUCH || sys_touch_read(&touch) != 1 ||
        touch.type != TOUCH_EVENT_DOWN || touch.x != 120 || touch.y != 240) {
        SDL_Log("Mouse press did not reach the syscall dispatcher");
        return 1;
    }

    sys_canvas_set_color(RGB(10, 10, 30));
    sys_canvas_clear();
    sys_canvas_set_color(RGB(255, 255, 255));
    sys_canvas_fill_circle(touch.x, touch.y, 12);
    sys_canvas_present();
    if (!check_pixel(renderer, 0, 0, RGB(10, 10, 30)) ||
        !check_pixel(renderer, 120, 240, RGB(255, 255, 255)) ||
        !check_pixel(renderer, 140, 240, RGB(10, 10, 30)) ||
        !check_pixel(renderer, 12, 12, RGB(255, 255, 255)) ||
        !check_pixel(renderer, 36, 12, RGB(0, 0, 0)) ||
        !check_pixel(renderer, 39, 12, RGB(255, 255, 255))) {
        return 1;
    }

    SDL_zero(event);
    event.type = SDL_MOUSEMOTION;
    event.motion.x = 200;
    event.motion.y = 300;
    if (sys_touch_read(&touch) != 0 || touch.type != TOUCH_EVENT_NONE ||
        SDL_PushEvent(&event) != 1 || sys_touch_read(&touch) != 1 ||
        touch.type != TOUCH_EVENT_MOVE || touch.x != 200 || touch.y != 300) {
        SDL_Log("Move failed: type=%d at %d,%d", touch.type, touch.x, touch.y);
        return 1;
    }
    sys_canvas_present();
    if (!check_pixel(renderer, 36, 12, RGB(255, 255, 255)) ||
        !check_pixel(renderer, 108, 21, RGB(0, 0, 0))) {
        SDL_Log("Coordinate overlay did not update");
        return 1;
    }

    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONUP;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = 220;
    event.button.y = 320;
    if (SDL_PushEvent(&event) != 1 || sys_touch_read(&touch) != 1 ||
        touch.type != TOUCH_EVENT_UP || touch.x != 220 || touch.y != 320 ||
        sys_touch_read(&touch) != 0 || touch.type != TOUCH_EVENT_NONE) {
        SDL_Log("Release failed: type=%d at %d,%d", touch.type, touch.x, touch.y);
        return 1;
    }

    /* Press and release within one frame must not disappear. */
    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = 40;
    event.button.y = 50;
    if (SDL_PushEvent(&event) != 1) return 1;
    event.type = SDL_MOUSEBUTTONUP;
    if (SDL_PushEvent(&event) != 1) return 1;
    sys_canvas_present();
    if (!sys_touch_read(&touch) || touch.type != TOUCH_EVENT_DOWN ||
        touch.x != 40 || touch.y != 50 ||
        !sys_touch_read(&touch) || touch.type != TOUCH_EVENT_UP ||
        touch.x != 40 || touch.y != 50 ||
        sys_touch_read(&touch) || touch.type != TOUCH_EVENT_NONE) {
        SDL_Log("Quick click failed: type=%d at %d,%d", touch.type, touch.x, touch.y);
        return 1;
    }
    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = 60;
    event.button.y = 70;
    if (SDL_PushEvent(&event) != 1) return 1;
    SDL_zero(event);
    event.type = SDL_WINDOWEVENT;
    event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    if (SDL_PushEvent(&event) != 1 ||
        !sys_touch_read(&touch) || touch.type != TOUCH_EVENT_DOWN ||
        !sys_touch_read(&touch) || touch.type != TOUCH_EVENT_UP ||
        touch.x != 60 || touch.y != 70) return 1;

    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = -10;
    event.button.y = -20;
    if (SDL_PushEvent(&event) != 1 || !sys_touch_read(&touch) ||
        touch.type != TOUCH_EVENT_DOWN || touch.x != 0 || touch.y != 0) return 1;
    SDL_zero(event);
    event.type = SDL_MOUSEMOTION;
    event.motion.x = width + 120;
    event.motion.y = height + 100;
    if (SDL_PushEvent(&event) != 1 || !sys_touch_read(&touch) ||
        touch.type != TOUCH_EVENT_MOVE || touch.x != width - 1 ||
        touch.y != height - 1) return 1;
    event.motion.x = width + 220;
    event.motion.y = height + 200;
    if (SDL_PushEvent(&event) != 1 || sys_touch_read(&touch) ||
        touch.type != TOUCH_EVENT_NONE || touch.x != width - 1 ||
        touch.y != height - 1) return 1;
    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONUP;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = -50;
    event.button.y = height + 100;
    if (SDL_PushEvent(&event) != 1 || !sys_touch_read(&touch) ||
        touch.type != TOUCH_EVENT_UP || touch.x != 0 ||
        touch.y != height - 1) return 1;
    sys_canvas_set_color(RGB(10, 10, 30));
    sys_canvas_clear();
    sys_canvas_present();
    if (!check_pixel(renderer, 120, 240, RGB(10, 10, 30))) {
        return 1;
    }

    test_window.w = 480;
    test_window.h = 800;
    touch.type = TOUCH_EVENT_DOWN;
    touch.x = 100;
    touch.y = 750;
    env_ListenTouch(&test_window, &touch);
    touch.type = TOUCH_EVENT_MOVE;
    touch.y = 650;
    env_ListenTouch(&test_window, &touch);
    if (test_window.h != 700 || test_window.w != 430) return 1;
    touch.type = TOUCH_EVENT_NONE;
    env_ListenTouch(&test_window, &touch);
    if (test_window.h != 700) return 1;
    touch.type = TOUCH_EVENT_UP;
    env_ListenTouch(&test_window, &touch);
    if (test_window.x != 0 || test_window.y != 0 ||
        test_window.w != 480 || test_window.h != 800) return 1;

    sim_set_coordinate_overlay(0);
    sim_net_attach();
    home_app_init();
    home_app_step();
    if (!check_pixel(renderer, 30, 66, RGB(30, 61, 67)) ||
        !check_pixel(renderer, 140, 66, RGB(30, 61, 67)) ||
        !check_pixel(renderer, 250, 66, RGB(30, 61, 67)) ||
        !check_pixel(renderer, 350, 66, RGB(13, 24, 29)) ||
        !check_pixel(renderer, 360, 66, RGB(30, 61, 67))) return 1;

    if (!test_network_refresh(renderer)) return 1;
    if (!test_usb_files(renderer)) { SDL_Log("USB Files workflow failed"); return 1; }
    if (!test_wifi_and_clock(renderer)) { SDL_Log("Wi-Fi join or network clock failed"); return 1; }

    if (!push_click(405, 66)) return 1;
    home_app_step();
    if (!check_pixel(renderer, 30, 60, RGB(30, 61, 67)) ||
        !check_pixel(renderer, 360, 66, RGB(13, 24, 29)) ||
        !check_pixel(renderer, 30, 200, RGB(13, 24, 29))) return 1;
    if (!push_click(40, 50)) return 1;
    home_app_step();
    if (!check_pixel(renderer, 360, 66, RGB(30, 61, 67))) return 1;

    SDL_zero(event);
    event.type = SDL_KEYDOWN;
    event.key.keysym.sym = SDLK_RETURN;
    if (SDL_PushEvent(&event) != 1) return 1;
    home_app_step();
    home_app_step();
    if (!check_pixel(renderer, 20, 12, RGB(33, 76, 74))) return 1;
    event.key.keysym.sym = SDLK_ESCAPE;
    if (SDL_PushEvent(&event) != 1) return 1;
    home_app_step();
    if (!check_pixel(renderer, 30, 66, RGB(30, 61, 67))) return 1;

    SDL_Quit();
    return 0;
}
