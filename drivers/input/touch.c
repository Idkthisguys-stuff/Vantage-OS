#include <vantage/drivers/touch.h>
#include <vantage/platform/pi4/i2c.h>
#include <vantage/platform/pi4/io.h>

#define REPORT_CONTACTS 5U
#define REPORT_HEADER 3U
#define CONTACT_BYTES 6U
#define QUEUE_SIZE (REPORT_CONTACTS + TOUCH_CONTACTS_MAX)

enum { CONTROLLER_DOWN, CONTROLLER_UP, CONTROLLER_CONTACT };

typedef struct {
    uint8_t down, missed;
    int x, y;
} Slot;

static Slot slots[TOUCH_CONTACTS_MAX];
static TouchContact queue[QUEUE_SIZE];
static uint32_t next, count;
static int last_x, last_y;

static void emit(TouchEventType type, uint32_t id, const Slot* slot) {
    queue[count++] = (TouchContact){ {type, slot->x, slot->y}, id };
}

static void poll(void) {
    uint8_t report[REPORT_HEADER + REPORT_CONTACTS * CONTACT_BYTES - 1];
    uint8_t seen[TOUCH_CONTACTS_MAX] = {0};
    if (!i2c_read_reg(TOUCH_ADDR, 0, report, sizeof(report))) return;
    uint32_t reported = report[2] & 0xf;
    if (reported > REPORT_CONTACTS) return;

    /* Poll only after draining the previous report. The queue has room for
     * one event per report entry and one missed release per known contact. */
    next = count = 0;
    for (uint32_t i = 0; i < reported; i++) {
        const uint8_t* p = report + REPORT_HEADER + CONTACT_BYTES * i;
        if ((p[0] >> 6) > CONTROLLER_CONTACT) return;
    }
    for (uint32_t i = 0; i < reported; i++) {
        const uint8_t* p = report + REPORT_HEADER + CONTACT_BYTES * i;
        uint32_t id = p[2] >> 4;
        if (id >= TOUCH_CONTACTS_MAX || seen[id]) continue;
        seen[id] = 1;
        Slot* slot = &slots[id];
        slot->missed = 0;
        if ((p[0] >> 6) == CONTROLLER_UP) {
            if (slot->down) emit(TOUCH_EVENT_UP, id, slot);
            slot->down = 0;
            continue;
        }

        int raw_x = ((p[0] & 0xf) << 8) | p[1];
        int raw_y = ((p[2] & 0xf) << 8) | p[3];
        int panel_x = TOUCH_FLIP_X ? TOUCH_WIDTH - 1 - raw_x : raw_x;
        int panel_y = TOUCH_FLIP_Y ? TOUCH_HEIGHT - 1 - raw_y : raw_y;
        int x = TOUCH_HEIGHT - 1 - panel_y, y = panel_x;
        uint32_t moved = x != slot->x || y != slot->y;
        slot->x = x;
        slot->y = y;
        if (!slot->down) emit(TOUCH_EVENT_DOWN, id, slot);
        else if (moved) emit(TOUCH_EVENT_MOVE, id, slot);
        slot->down = 1;
    }

    for (uint32_t id = 0; id < TOUCH_CONTACTS_MAX; id++) {
        Slot* slot = &slots[id];
        if (slot->down && !seen[id] && ++slot->missed >= 2) {
            slot->down = 0;
            emit(TOUCH_EVENT_UP, id, slot);
        }
    }
}

uint32_t touch_init(void) {
    for (uint32_t id = 0; id < TOUCH_CONTACTS_MAX; id++) slots[id] = (Slot){0};
    next = count = 0;
    last_x = last_y = 0;
    i2c_init();
    uart_writeText("[OK] Vantage touch driver...\n");
    return i2c_probe(TOUCH_ADDR);
}

uint32_t touch_read(TouchContact* contact) {
    if (next == count) poll();
    if (next == count) {
        *contact = (TouchContact){ {TOUCH_EVENT_NONE, last_x, last_y}, 0 };
        return 0;
    }
    *contact = queue[next++];
    last_x = contact->event.x;
    last_y = contact->event.y;
    return 1;
}
