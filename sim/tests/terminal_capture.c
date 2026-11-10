/* Capture actual SDL app frames while exercising persistent file syscalls. */
#include <vantage/kernel/window_manager.h>
#include <vantage/kernel/files.h>
#include <vantage/user.h>
#include "user/apps/terminal/terminal.h"
#include "../platform/display_sim.h"
#include "../platform/storage_host.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s (%s)\n", __LINE__, #c, SDL_GetError()); exit(1); } } while (0)
static const char document[] =
    "Preempted processes and persistent folders.\n"
    "VSTORE3: 256 entries, 255-byte paths, 1 MiB files.\n";

static void text(const char* input) {
    while (*input) {
        SDL_Event event;
        size_t count = strlen(input);
        if (count > sizeof(event.text.text) - 1) count = sizeof(event.text.text) - 1;
        SDL_zero(event); event.type = SDL_TEXTINPUT;
        memcpy(event.text.text, input, count);
        CHECK(SDL_PushEvent(&event) == 1);
        terminal_app_step(); input += count;
    }
}
static void command(const char* input) {
    SDL_Event event;
    text(input);
    SDL_zero(event); event.type = SDL_KEYDOWN; event.key.keysym.sym = SDLK_RETURN;
    CHECK(SDL_PushEvent(&event) == 1); terminal_app_step();
}
static void tap(int x, int y) {
    SDL_Event event;
    SDL_zero(event); event.type = SDL_MOUSEBUTTONDOWN;
    event.button.button = SDL_BUTTON_LEFT; event.button.x = x; event.button.y = y;
    CHECK(SDL_PushEvent(&event) == 1);
    event.type = SDL_MOUSEBUTTONUP; CHECK(SDL_PushEvent(&event) == 1);
    terminal_app_step();
}
static void capture(const char* prefix, const char* name) {
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
static void verify(void) {
    unsigned char buffer[sizeof(document)];
    int64_t handle = sys_file_open("/ram/docs/notes/roadmap", FILE_READ);
    CHECK(handle > 0);
    CHECK(sys_file_read((uint64_t)handle, buffer, sizeof(buffer)) == sizeof(document) - 1);
    CHECK(!memcmp(buffer, document, sizeof(document) - 1));
    CHECK(sys_file_close((uint64_t)handle) == 0);
    FileDirectoryLong entry = {0}; memcpy(entry.path, "/ram/empty", 11);
    CHECK(sys_file_directory_long(&entry) == 0 && entry.type == FILE_DIRECTORY);
}
int main(int argc, char** argv) {
    CHECK(argc == 4);
    CHECK(sim_storage_open(argv[2], 0) == 0);
    env_Init(480, 800); sim_set_coordinate_overlay(0);
    terminal_app_init(); terminal_app_step();
    if (!strcmp(argv[1], "write")) {
        command("mkdir empty"); command("mkdir docs"); command("mkdir docs/notes");
        command("cd docs/notes"); command("edit roadmap");
        command("Preempted processes and persistent folders.");
        command("VSTORE3: 256 entries, 255-byte paths, 1 MiB files.");
        tap(30, 440); capture(argv[3], "editor");
        tap(150, 440); verify();
        command("clear"); command("pwd"); command("ls"); command("cat roadmap");
        capture(argv[3], "folders");
        puts("PASS: actual SDL editor saved nested file through file syscalls");
    } else {
        CHECK(!strcmp(argv[1], "read")); verify();
        command("clear"); command("ls /ram"); command("cd docs/notes");
        command("pwd"); command("cat roadmap"); capture(argv[3], "reboot");
        puts("PASS: fresh simulator process read exact file bytes and empty directory");
    }
    CHECK(files_unmount() == 0); SDL_Quit();
    return 0;
}
