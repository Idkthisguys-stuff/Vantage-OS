#include "../includes/environment/windowManager.h"
#include "../includes/environment/user_lib.h"

static unsigned char user_stack[4096] __attribute__((aligned(16)));
void enter_user_space(void (*func)(void), void* stack_top);

void user_task() {
    int touch_data[2];

    while (1) {
        uint32_t touching = sys_touch_read(touch_data);

        int tx = touch_data[0];
        int ty = touch_data[1];

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
        int* user_buffer = (int*)arg0;
        int raw_x = 0, raw_y = 0;   
        uint32_t touching = touch_read(&raw_x, &raw_y);
        user_buffer[0] = raw_x;
        user_buffer[1] = raw_y;
        return touching;
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

void main() {
    env_Init(480, 800);

    void* stack_top = (void*)&user_stack[4096];
    enter_user_space(user_task, stack_top);

    while (1) {
        // IDLE
    }
}