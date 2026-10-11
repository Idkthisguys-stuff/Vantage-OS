// generated animation code for display testing -_-

#include "../includes/graphics.h"
#include "../includes/display.h"

#define FRAME_US 16667
#define MICRO 1000000LL
#define SPEED_MIN 200
#define SPEED_MAX 700

static uint32_t rngState;

static uint32_t rand_next(void) {
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return rngState;
}

static int64_t rand_speed(void) {
    return SPEED_MIN + rand_next() % (SPEED_MAX - SPEED_MIN + 1);
}

void main() {
    canvas_init(800, 480);

    rngState = time_us() | 1;

    int64_t x = 100 * MICRO;
    int64_t y = 100 * MICRO;
    int64_t vx = rand_speed();
    int64_t vy = rand_speed();
    uint32_t last = time_us();

    while (1) {
        while (time_us() - last < FRAME_US);

        uint32_t now = time_us();
        uint32_t dt = now - last;
        last = now;

        x += vx * dt;
        y += vy * dt;

        if (x < 30 * MICRO) {
            x = 30 * MICRO;
            vx = rand_speed();
        }
        if (x > 770 * MICRO) {
            x = 770 * MICRO;
            vx = -rand_speed();
        }
        if (y < 30 * MICRO) {
            y = 30 * MICRO;
            vy = rand_speed();
        }
        if (y > 450 * MICRO) {
            y = 450 * MICRO;
            vy = -rand_speed();
        }

        canvas_set_color(RGB(10, 10, 30));
        canvas_clear();

        canvas_set_color(RGB(255, 0, 0));
        canvas_fill_circle((int)(x / MICRO), (int)(y / MICRO), 30);

        canvas_present();
    }
}