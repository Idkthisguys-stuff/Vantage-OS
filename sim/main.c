#include <vantage/kernel/window_manager.h>
#include <SDL2/SDL.h>
#include "platform/storage_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/display_sim.h"
#include "platform/net_sim.h"

void user_task(void);

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    const char* storage = getenv("VANTAGE_STORAGE_IMAGE");
    if (storage) {
        int result = sim_storage_open(storage, 0);
        if (result < 0) { fprintf(stderr, "Storage image mount failed (%d)\n", result); return 1; }
        if (result) fprintf(stderr, "Storage recovered one valid snapshot; inspect the image\n");
    }

    /* The simulator's Wi-Fi is a built-in virtual network, not the host's. */
    sim_net_attach();
    const char* ethernet = getenv("VANTAGE_SIM_ETHERNET");
    if (ethernet && !strcmp(ethernet, "1")) sim_net_set_cable(1);
    env_Init(480, 800);
    sim_set_coordinate_overlay(0);
    SDL_SetWindowTitle(SDL_GetWindowFromID(1), "Vantage-OS Terminal");
    user_task();
    return 0;
}
