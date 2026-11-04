#include "../includes/env/windowManager.h"
#include <SDL2/SDL.h>

void user_task(void);

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    env_Init(480, 800);
    user_task();
    return 0;
}
