#include "../includes/env/windowManager.h"
#include "../user/include/user_lib.h"

#ifndef VANTAGE_SIM
#include "../includes/io.h"
#include "../kernel/fs/include/fs.h"

#include "../includes/sd.h"

static unsigned char user_stack[4096] __attribute__((aligned(16)));
void enter_user_space(void (*func)(void), void* stack_top);
#endif

void user_task() {
    TouchEvent touch;
    uint32_t touching = 0;
    int tx = 0;
    int ty = 0;

    while (1) {
        if (sys_touch_read(&touch)) {
            tx = touch.x;
            ty = touch.y;
            touching = touch.type != TOUCH_EVENT_UP;
        }

        // All rendering goes safely through syscalls now!
        sys_canvas_set_color(RGB(10, 10, 30));
        sys_canvas_clear();

        if (touching) {
            sys_canvas_set_color(RGB(255, 255, 255));
            sys_canvas_fill_circle(tx, ty, 12);
        }

        sys_canvas_present();
    }
}

uint64_t handle_sys_call(uint64_t syscall_id, uint64_t arg0) {
    if (syscall_id == 1) {
        fb_wait_frame();
        return touch_read((TouchEvent*)arg0);
    }
    else if (syscall_id == 2) {
        // Set Color (arg0 is the color value)
        canvas_set_color((uint32_t)arg0);
    }
    else if (syscall_id == 3) {
        // Clear Canvas
        canvas_clear();
    }
    else if (syscall_id == 4) {
        // Fill Circle (arg0 is pointer to struct circle_args)
        struct circle_args* args = (struct circle_args*)arg0;
        canvas_fill_circle(args->x, args->y, args->r);
    }
    else if (syscall_id == 5) {
        // Present Canvas
        canvas_present();
    }

    return 0;
}

#ifndef VANTAGE_SIM
void main() {
    uart_init();
    env_Init(480, 800);

    if (sd_init() == SD_OK) {
        uart_writeText("[OK] SD DRIVER INITIALIZED! (main)\n");
    }
   
    uart_writeText("[OK] Vantage booted!...\n");

    void* stack_top = (void*)&user_stack[4096];
    enter_user_space(user_task, stack_top);

    while (1) {
        // IDLE
    }
}
#endif
