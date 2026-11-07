#include "../includes/env/windowManager.h"
#include "../user/include/user_lib.h"
#include "../includes/platform.h"

#ifndef VANTAGE_SIM
#include "../includes/io.h"
#include "../kernel/fs/include/fat.h"

#include "../includes/sd.h" 
#include "../includes/mmu.h"
#include "../includes/parallel.h"
#include "../includes/memory.h"

extern unsigned char _end[];

static unsigned char user_stack[4096]
    __attribute__((section(".user.bss"), aligned(16)));
void enter_user_space(void (*func)(void), void* stack_top);
unsigned int secondary_core_start(unsigned int core_id);
#endif

extern unsigned char _end;

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
        if (arg0 % sizeof(uint32_t) ||
            !user_buffer_valid((uintptr_t)arg0, sizeof(TouchEvent))) return 0;
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
        if (arg0 % sizeof(uint32_t) ||
            !user_buffer_valid((uintptr_t)arg0, sizeof(struct circle_args))) return 0;
        const struct circle_args args = *(const struct circle_args*)arg0;
        canvas_fill_circle(args.x, args.y, args.r);
    }
    else if (syscall_id == 5) {
        // Present Canvas
        canvas_present();
    }

    return 0;
}

#ifndef VANTAGE_SIM
void main(uintptr_t dtb) {
    uart_init();
    if (!mm_prepare(dtb, (uintptr_t)_end))
        mm_halt("[ERR] Firmware memory discovery failed; boot halted\n");
    uart_writeText("[OK] Firmware RAM and reservations discovered\n");
    for (unsigned int core = 1; core < 4; core++) {
        if (secondary_core_start(core)) {
            if (core == 1) uart_writeText("[OK] Core 1 entry reached\n");
            if (core == 2) uart_writeText("[OK] Core 2 entry reached\n");
            if (core == 3) uart_writeText("[OK] Core 3 entry reached\n");
        } else {
            if (core == 1) uart_writeText("[ERR] Core 1 startup unconfirmed; boot halted\n");
            if (core == 2) uart_writeText("[ERR] Core 2 startup unconfirmed; boot halted\n");
            if (core == 3) uart_writeText("[ERR] Core 3 startup unconfirmed; boot halted\n");
            for (;;) __asm__ volatile("wfe");
        }
    }
    env_Init(480, 800);
    if (!mmu_is_enabled() || !parallel_init() || !parallel_self_test())
        parallel_halt("[ERR] Four-core parallel initialization failed; boot halted\n");
    uart_writeText("[OK] Four-core parallel self-test passed\n");

    if (sd_init() == SD_OK) {
        uart_writeText("[OK] SD DRIVER INITIALIZED! (main)\n");

        if (fat_getPartition()) {
            fat_listDirectory();
        } else {
            uart_writeText("[ERR] FAT partition is not found?? \n");
        }
    }
   
    uart_writeText("[OK] Vantage booted!...\n");

    void* stack_top = (void*)&user_stack[4096];
    enter_user_space(user_task, stack_top);

    while (1) {
        // IDLE
    }
}
#endif
