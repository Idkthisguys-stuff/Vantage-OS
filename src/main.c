#include "../includes/environment/windowManager.h"

static Window window;

void main() {
    env_Init(480, 800);

    window.w = 480;
    window.h = 800;
    window.x = (480 - window.w) / 2;
    window.y = (800 - window.h) / 2;
    window.bgColor = RGB(0, 0, 125);

    while (1) {
        canvas_clear();
        env_DrawWindow(&window);

        canvas_present();
    }
}