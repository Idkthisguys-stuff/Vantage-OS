#include <vantage/drivers/touch.h>
#include <vantage/platform/pi4/i2c.h>
#include <vantage/platform/pi4/io.h>

#define TOUCH_POINTS 10
typedef struct {
    uint8_t down, miss;
    int x, y;
} Slot;
static Slot slot[TOUCH_POINTS];
static TouchEvent q[16];
static uint32_t qh, qt;

static void qpush(uint8_t type, uint8_t id, int x, int y) {
    uint32_t n = (qh + 1) % 16;
    if (n == qt) return;
    q[qh] = (TouchEvent){
        .type = type,
        .id = id,
        .x = x,
        .y = y
    };
    qh = n;
}

static void poll(void) {
    uint8_t buf[32], seen[TOUCH_POINTS] = {0};
    if (!i2c_read_reg(TOUCH_ADDR, 0, buf, sizeof(buf))) return;
    uint8_t count = buf[2] & 0xF;
    if (count > 5) count = 5;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t* p = buf + 3 + 6 * i;
        uint8_t id = p[2] >> 4;
        if (id >= TOUCH_POINTS) continue;
        int rawX = ((p[0] & 0xF) << 8) | p[1];
        int rawY = ((p[2] & 0xF) << 8) | p[3];
        int panelX = TOUCH_FLIP_X ? TOUCH_WIDTH - 1 - rawX : rawX;
        int panelY = TOUCH_FLIP_Y ? TOUCH_HEIGHT - 1 - rawY : rawY;
        int x = TOUCH_HEIGHT - 1 - panelY, y = panelX;
        seen[id] = 1;
        Slot* s = &slot[id];
        s->miss = 0;
        if (!s->down) { s->down = 1; qpush(TOUCH_EVENT_DOWN, id, x, y); }
        else if (x != s->x || y != s->y) {
            int dx = x - s->x, dy = y - s->y;
            if (dx*dx + dy*dy > 40*40) {
                qpush(TOUCH_EVENT_UP, id, s->x, s->y);
                qpush(TOUCH_EVENT_DOWN, id, x, y);
            } else qpush(TOUCH_EVENT_MOVE, id, x, y);
        }
        s->x = x; s->y = y;
    }
    for (uint8_t id = 0; id < TOUCH_POINTS; id++) {
        if (slot[id].down && !seen[id] && ++slot[id].miss >= 2) {
            slot[id].down = 0;
            qpush(TOUCH_EVENT_UP, id, slot[id].x, slot[id].y);
        }
    }
}

uint32_t touch_init(void) {
    for (uint32_t i = 0; i < TOUCH_POINTS; i++) slot[i] = (Slot){0};
    qh = qt = 0;
    i2c_init();
    uart_writeText("[OK] Vantage touch driver...\n");
    return i2c_probe(TOUCH_ADDR);
}

uint32_t touch_read(TouchEvent* r) {
    if (qh == qt) poll();
    if (qh == qt) return 0;
    *r = q[qt]; qt = (qt + 1) % 16;
    return 1;
}
