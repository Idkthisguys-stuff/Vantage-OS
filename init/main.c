#include "../includes/env/windowManager.h"
#include "../user/include/user_lib.h"
#include "../includes/platform.h"
#include "../includes/console.h"

#ifndef VANTAGE_SIM
#include "../includes/io.h"
#include "../kernel/fs/include/fs.h"

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

void user_task(void);

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
    else if (syscall_id == 6) {
        if (arg0 % sizeof(uint32_t) || !user_buffer_valid((uintptr_t)arg0, sizeof(KeyEvent))) return 0;
        return key_read((KeyEvent*)(uintptr_t)arg0);
    }
    else if (syscall_id == 7) {
        if (arg0 % sizeof(uint32_t) || !user_buffer_valid((uintptr_t)arg0, sizeof(TextRow))) return 0;
        const TextRow row = *(const TextRow*)(uintptr_t)arg0;
        return canvas_text_row(&row);
    }
    else if (syscall_id == 8) {
        ConsoleInfo info = {0};
        if (arg0 % sizeof(uint64_t) || !user_buffer_valid((uintptr_t)arg0, sizeof(info))) return 0;
        info.columns = canvas_text_columns();
        info.rows = canvas_text_rows();
#ifdef VANTAGE_SIM
        info.memory_kind = CONSOLE_MEMORY_HOST;
        info.host_backbuffer_bytes = sim_backbuffer_bytes();
#else
        if (mm_stats(&info.memory)) info.memory_kind = CONSOLE_MEMORY_KERNEL;
#endif
        *(ConsoleInfo*)(uintptr_t)arg0 = info;
        return 1;
    }
    else if (syscall_id == 9) fb_wait_frame();
    else if (syscall_id == 10) {
        if (arg0 % sizeof(uint32_t) || !user_buffer_valid((uintptr_t)arg0, sizeof(ConsoleWrite))) return 0;
        const ConsoleWrite request = *(const ConsoleWrite*)(uintptr_t)arg0;
        if (request.length > CONSOLE_WRITE_MAX) return 0;
#ifndef VANTAGE_SIM
        uart_write_bytes(request.text, request.length);
#endif
        return request.length;
    }

    else if (syscall_id == 11) {
        if (arg0 % sizeof(uint32_t) || !user_buffer_valid((uintptr_t)arg0, sizeof(CanvasButton))) return 0;
        const CanvasButton button = *(const CanvasButton*)(uintptr_t)arg0;
        return canvas_button(&button);
    }
    else if (syscall_id == 12) return time_us();

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

    /*if (!sd_init()) {
        uart_writeText("[OK] SD DRIVER INITIALIZED!\n");
    }*/
   
    uart_writeText("[OK] Vantage booted!...\n");

    void* stack_top = (void*)&user_stack[4096];
    enter_user_space(user_task, stack_top);

    while (1) {
        // IDLE
    }
}
#endif
