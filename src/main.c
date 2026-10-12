// generated animation code for display testing -_-

#include "../includes/graphics.h"
#include "../includes/display.h"
#include "../includes/touch.h"
#include "../includes/i2c.h"

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

    uint32_t touchOk = touch_init();
    uint32_t panelOk = i2c_probe(PANEL_ADDR);

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

        int tx = 0;
        int ty = 0;
        uint32_t touching = touchOk && touch_read(&tx, &ty);

        if (touching) {
            x = (int64_t)tx * MICRO;
            y = (int64_t)ty * MICRO;
        } else {
            x += vx * dt;
            y += vy * dt;
        }

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

        canvas_set_color(touchOk ? RGB(0, 200, 0) : RGB(200, 0, 0));
        canvas_fill_rect(10, 10, 30, 30);
        canvas_set_color(panelOk ? RGB(0, 200, 0) : RGB(200, 0, 0));
        canvas_fill_rect(50, 10, 30, 30);

        canvas_set_color(touching ? RGB(0, 255, 0) : RGB(255, 0, 0));
        canvas_fill_circle((int)(x / MICRO), (int)(y / MICRO), 30);

        canvas_present();
    }
}