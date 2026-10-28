#include "../../includes/environment/user_lib.h"

#define SIMD_CLOBBERS "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", \
                      "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", \
                      "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31"

static inline uint64_t syscall(uint64_t id, uint64_t arg) {
    register uint64_t r0 __asm__("x0") = arg;
    register uint64_t r8 __asm__("x8") = id;

    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r8) : "memory", SIMD_CLOBBERS);

    return r0;
}

uint32_t sys_touch_read(int* buf) {
    return (uint32_t)syscall(1, (uint64_t)buf);
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