#include "../../includes/mmu.h"
#include "../../includes/platform.h"
#include "../../user/include/user_lib.h"
#include <stdio.h>

static uint64_t level1[4];
static uint64_t level2[4][512];
static uint32_t user_words[64];
static unsigned int reads, circles;

uint64_t handle_sys_call(uint64_t id, uint64_t arg);

uint32_t user_buffer_valid(uintptr_t buffer, size_t size) {
    return user_region_contains((uintptr_t)user_words,
                                (uintptr_t)(user_words + 64), buffer, size);
}

void fb_wait_frame(void) {}
uint32_t touch_read(TouchEvent* event) {
    reads++;
    event->type = TOUCH_EVENT_DOWN;
    event->x = 12;
    event->y = 34;
    return 1;
}
void canvas_set_color(uint32_t color) { (void)color; }
void canvas_clear(void) {}
void canvas_present(void) {}
void canvas_fill_circle(int x, int y, int r) {
    if (x == 12 && y == 34 && r == 5) circles++;
}
uint32_t sys_touch_read(TouchEvent* event) { (void)event; return 0; }
void sys_canvas_set_color(uint32_t color) { (void)color; }
void sys_canvas_clear(void) {}
void sys_canvas_fill_circle(int x, int y, int r) { (void)x; (void)y; (void)r; }
void sys_canvas_present(void) {}

int main(void) {
    uintptr_t start = (uintptr_t)user_words;
    uintptr_t end = (uintptr_t)(user_words + 64);
    TouchEvent kernel_event = { TOUCH_EVENT_NONE, 91, 92 };
    struct circle_args* args = (struct circle_args*)user_words;
    unsigned int g, i;

    mmu_populate_tables(level1, level2, 0x3e000000, 0x400000, 0x400000, 0x600000, 0x600000);
    for (g = 0; g < 4; g++) {
        if (level1[g] != ((uint64_t)(uintptr_t)level2[g] | 3)) return 1;
        for (i = 0; i < 512; i++) {
            unsigned int ap = (unsigned int)((level2[g][i] >> 6) & 3);
            unsigned int expected = g == 0 && i == 0 ? 3 : g == 0 && i == 2 ? 1 : 0;
            if (ap != expected) return 1;
            if (g == 0 && i == 3 &&
                (((level2[g][i] >> 2) & 7) != 0 || ((level2[g][i] >> 8) & 3) != 2)) return 1;
            if ((g || i) && !(level2[g][i] & (1ULL << 54))) return 1;
        }
    }
    /* Even overlapping caller ranges cannot make the transition gate user RW
     * or framebuffer Normal memory. Its reserved Device mapping takes priority. */
    mmu_populate_tables(level1, level2, 0x600000, 0x200000, 0x600000, 0x800000, 0x600000);
    if (((level2[0][3] >> 2) & 7) || ((level2[0][3] >> 6) & 3)) return 1;
    if (!user_region_contains(start, end, start, sizeof(user_words)) ||
        user_region_contains(start, end, start - 1, 1) ||
        user_region_contains(start, end, end, 1) ||
        user_region_contains(start, end, end - 4, 8) ||
        user_region_contains(end, start, start, 1) ||
        user_region_contains(start, end, (uintptr_t)-4, 8)) return 1;

    handle_sys_call(1, 0);
    handle_sys_call(1, (uint64_t)(uintptr_t)&kernel_event);
    handle_sys_call(1, (uint64_t)(end - 4));
    handle_sys_call(1, (uint64_t)(start + 1));
    if (reads || kernel_event.type != TOUCH_EVENT_NONE || kernel_event.x != 91) return 1;
    if (handle_sys_call(1, (uint64_t)start) != 1 || reads != 1 ||
        ((TouchEvent*)user_words)->x != 12) return 1;

    args->x = 12; args->y = 34; args->r = 5;
    handle_sys_call(4, (uint64_t)(uintptr_t)&kernel_event);
    handle_sys_call(4, 0);
    handle_sys_call(4, (uint64_t)(end - 4));
    handle_sys_call(4, (uint64_t)(start + 1));
    if (circles) return 1;
    handle_sys_call(4, (uint64_t)start);
    if (circles != 1) return 1;
    puts("PASS: kernel mappings, user stack region, buffer boundaries and syscall pointer checks");
    return 0;
}
