#include "../include/user_lib.h"

#ifdef VANTAGE_SIM
/* Desktop builds use the same dispatcher without an ARM exception. */
uint64_t handle_sys_call(uint64_t syscall_id, uint64_t arg0);

static inline uint64_t syscall(uint64_t id, uint64_t arg) {
    return handle_sys_call(id, arg);
}
#else
#define SIMD_CLOBBERS "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", \
                      "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", \
                      "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31"

static inline uint64_t syscall(uint64_t id, uint64_t arg) {
    register uint64_t r0 __asm__("x0") = arg;
    register uint64_t r8 __asm__("x8") = id;

    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r8) : "memory", SIMD_CLOBBERS);

    return r0;
}
#endif

uint32_t sys_touch_read(TouchEvent* event) {
    return (uint32_t)syscall(1, (uint64_t)event);
}

void sys_canvas_set_color(uint32_t color) {
    syscall(2, color);
}

void sys_canvas_clear(void) {
    syscall(3, 0);
}

void sys_canvas_fill_circle(int x, int y, int r) {
    struct circle_args args = { .x = x, .y = y, .r = r };
    syscall(4, (uint64_t)&args);
}

void sys_canvas_present(void) {
    syscall(5, 0);
}

uint32_t sys_key_read(KeyEvent* event) {
    return (uint32_t)syscall(6, (uint64_t)(uintptr_t)event);
}

uint32_t sys_canvas_text_row(const TextRow* row) {
    return (uint32_t)syscall(7, (uint64_t)(uintptr_t)row);
}

uint32_t sys_canvas_button(const CanvasButton* button) {
    return (uint32_t)syscall(11, (uint64_t)(uintptr_t)button);
}

uint32_t sys_console_info(ConsoleInfo* info) {
    return (uint32_t)syscall(8, (uint64_t)(uintptr_t)info);
}

void sys_frame_wait(void) {
    syscall(9, 0);
}

uint32_t sys_time_us(void) {
    return (uint32_t)syscall(12, 0);
}

void sys_console_write(const char* text, size_t length) {
    if (!text) return;
    while (length) {
        ConsoleWrite request = {0};
        uint32_t i;
        request.length = length < CONSOLE_WRITE_MAX ? (uint32_t)length : CONSOLE_WRITE_MAX;
        for (i = 0; i < request.length; i++) request.text[i] = text[i];
        if (syscall(10, (uint64_t)(uintptr_t)&request) != request.length) return;
        text += request.length;
        length -= request.length;
    }
}
