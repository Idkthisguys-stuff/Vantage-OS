#include "../includes/env/windowManager.h"
#include <SDL2/SDL.h>
#include "platform/display_sim.h"

void user_task(void);

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    env_Init(480, 800);
    sim_set_coordinate_overlay(0);
    SDL_SetWindowTitle(SDL_GetWindowFromID(1), "Vantage-OS Terminal");
    user_task();
    return 0;
}
