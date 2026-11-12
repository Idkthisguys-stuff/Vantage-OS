/* Drive the real terminal's net commands through scan, join, DHCP, status,
 * lookup, ping, HTTP, saved networks and leave over the simulator's virtual
 * network, optionally capturing SDL frames. With VANTAGE_STORAGE_IMAGE set it
 * also saves a download and the saved-networks file. */
#include <vantage/kernel/window_manager.h>
#include <vantage/user.h>
#include "user/apps/terminal/terminal.h"
#include "../platform/display_sim.h"
#include "../platform/net_peer.h"
#include "../platform/net_sim.h"
#include "../platform/storage_host.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s (%s)\n", __LINE__, #c, SDL_GetError()); exit(1); } } while (0)

static const char* prefix;

static void step(void) { terminal_app_step(); sys_frame_wait(); }

static void text(const char* input) {
    while (*input) {
        SDL_Event event;
        size_t count = strlen(input);
        if (count > sizeof(event.text.text) - 1) count = sizeof(event.text.text) - 1;
        SDL_zero(event); event.type = SDL_TEXTINPUT;
        memcpy(event.text.text, input, count);
        CHECK(SDL_PushEvent(&event) == 1);
        step(); input += count;
    }
}

static void enter(void) {
    SDL_Event event;
    SDL_zero(event); event.type = SDL_KEYDOWN; event.key.keysym.sym = SDLK_RETURN;
    CHECK(SDL_PushEvent(&event) == 1); step();
}

static void command(const char* input) { text(input); enter(); }

static void wait_ms(uint32_t ms) {
    uint32_t start = SDL_GetTicks();
    while (SDL_GetTicks() - start < ms) step();
}

#define WAIT_FOR(condition, ms) do { uint32_t start_ = SDL_GetTicks(); \
    while (!(condition)) { CHECK(SDL_GetTicks() - start_ < (ms)); step(); } } while (0)

static void capture(const char* name) {
    if (!prefix) return;
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s.%s.bmp", prefix, name);
    CHECK(length > 0 && (size_t)length < sizeof(path));
    SDL_Window* window = SDL_GetWindowFromID(1);
    SDL_Renderer* renderer = window ? SDL_GetRenderer(window) : NULL;
    CHECK(renderer);
    SDL_Surface* frame = SDL_CreateRGBSurfaceWithFormat(0, 480, 800, 32, SDL_PIXELFORMAT_XRGB8888);
    CHECK(frame);
    CHECK(SDL_RenderReadPixels(renderer, NULL, frame->format->format, frame->pixels, frame->pitch) == 0);
    CHECK(SDL_SaveBMP(frame, path) == 0);
    SDL_FreeSurface(frame);
}

static NetStatus status(void) {
    NetStatus s;
    CHECK(sys_net_status(&s) == 0);
    return s;
}

/* Kernel profile count, or -1 when the kernel has no saved-network support. */
static int profiles(void) {
    NetProfile entry = {0};
    int count = 0, result;
    entry.operation = NET_PROFILE_LIST;
    while ((result = sys_net_profile(&entry)) == 1) { count++; entry.operation = NET_PROFILE_LIST; }
    if (result == NET_UNSUPPORTED) return -1;
    CHECK(result == 0);
    return count;
}

static uint32_t read_file(const char* path, char* data, uint32_t capacity) {
    int64_t handle = sys_file_open(path, FILE_READ);
    uint32_t size = 0;
    int count;
    if (handle < 0) return 0;
    while ((count = sys_file_read((uint64_t)handle, data + size, capacity - size < FILE_IO_MAX ? capacity - size : FILE_IO_MAX)) > 0)
        size += (uint32_t)count;
    CHECK(count == 0 && sys_file_close((uint64_t)handle) == 0);
    return size;
}

int main(int argc, char** argv) {
    static char data[32768];
    const char* image = getenv("VANTAGE_STORAGE_IMAGE");
    prefix = argc > 1 ? argv[1] : NULL;
    if (image) CHECK(sim_storage_open(image, 0) >= 0);
    sim_net_attach();
    env_Init(480, 800);
    sim_set_coordinate_overlay(0);
    terminal_app_init();
    step();

    command("net scan");
    wait_ms(1500);
    command("net join VantageNet");
    text(NET_PEER_PASSPHRASE);
    capture("passphrase");
    enter();
    WAIT_FOR(status().configuration == NET_ADDRESS_BOUND, 10000);
    wait_ms(300);
    command("net");
    capture("status");
    NetStatus s = status();
    CHECK(s.link == NET_LINK_UP && s.address == NET_PEER_CLIENT && s.gateway == NET_PEER_ROUTER);
    int saved = profiles();
    if (saved >= 0) {
        CHECK(saved == 1);
        if (image) {
            uint32_t size = read_file("/ram/.netprofiles", data, sizeof(data) - 1);
            data[size] = 0;
            CHECK(!strcmp(data, "vantage-networks 1\n0 56616e746167654e6574 76616e746167652d64656d6f\n"));
        }
    }
    command("net saved");

    command("clear");
    command("net ping vantage.test 3");
    WAIT_FOR(net_peer_stats()->echo_replies == 3, 8000);
    wait_ms(500);
    command("net lookup www.vantage.test");
    wait_ms(500);
    command("net lookup missing.vantage.test");
    wait_ms(500);
    capture("ping");

    command("clear");
    command("net get http://vantage.test/");
    WAIT_FOR(net_peer_stats()->http_requests == 1, 8000);
    wait_ms(1500);
    capture("get");
    command("clear");
    command("net get https://vantage.test/");
    WAIT_FOR(net_peer_stats()->http_requests == 2, 15000);
    wait_ms(1500);
    capture("get-https");
    if (image) {
        command("net get http://vantage.test/large -o large.txt");
        WAIT_FOR(net_peer_stats()->http_requests == 3, 8000);
        wait_ms(1500);
        uint32_t size = read_file("/ram/large.txt", data, sizeof(data));
        CHECK(size > 20000 && size < sizeof(data));
        CHECK(!memcmp(data, "line 00000 of the large test document\n", 38));
        CHECK(!memcmp(data + size - 38, "line 00526 of the large test document\n", 38));
    }
    CHECK(net_peer_stats()->dns_queries >= 3 && !net_peer_stats()->invalid);

    command("net forget VantageNet");
    if (saved >= 0) CHECK(profiles() == 0);
    if (saved >= 0 && image) CHECK(read_file("/ram/.netprofiles", data, sizeof(data)) == 19);
    command("net leave");
    wait_ms(200);
    CHECK(status().link == NET_LINK_DOWN && net_peer_stats()->dhcp_releases == 1);
    printf("PASS: real terminal scanned, joined with a masked passphrase, pinged, looked up and fetched HTTP%s%s\n",
           saved >= 0 ? "; saved and forgot the network" : "", image ? "; saved a download" : "");
    SDL_Quit();
    if (image) CHECK(sim_storage_close() == 0);
    return 0;
}
