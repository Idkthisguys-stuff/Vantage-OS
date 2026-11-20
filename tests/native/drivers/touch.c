#include <vantage/drivers/touch.h>
#include <vantage/platform/pi4/i2c.h>
#include <vantage/platform/pi4/io.h>
#include <stdio.h>
#include <string.h>

static uint8_t sample[32];
static int read_ok = 1;

void i2c_init(void) {}
uint32_t i2c_probe(uint8_t addr) { return addr == TOUCH_ADDR; }
void uart_writeText(const char* text) { (void)text; }
uint32_t i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t* data, uint32_t len) {
    if (!read_ok || addr != TOUCH_ADDR || reg != 0 || len != sizeof(sample)) {
        return 0;
    }
    memcpy(data, sample, sizeof(sample));
    return 1;
}

static void set_contact(int index, int id, int controller_event, int raw_x, int raw_y) {
    uint8_t* p = sample + 3 + 6 * index;
    p[0] = (uint8_t)((controller_event << 6) | ((raw_x >> 8) & 15));
    p[1] = (uint8_t)raw_x;
    p[2] = (uint8_t)((id << 4) | ((raw_y >> 8) & 15));
    p[3] = (uint8_t)raw_y;
}

static void set_sample(int count, int controller_event, int raw_x, int raw_y) {
    memset(sample, 0, sizeof(sample));
    sample[2] = (uint8_t)count;
    set_contact(0, 0, controller_event, raw_x, raw_y);
}

static int expect_contact(TouchEventType type, unsigned int id, int x, int y) {
    TouchContact contact = {0};
    uint32_t present = touch_read(&contact);
    TouchEvent event = contact.event;
    if (present != (uint32_t)(type != TOUCH_EVENT_NONE) || contact.id != id ||
        event.type != type || event.x != x || event.y != y) {
        fprintf(stderr, "got %d id %u (%d,%d), expected %d id %u (%d,%d)\n",
                event.type, contact.id, event.x, event.y, type, id, x, y);
        return 0;
    }
    return 1;
}

static int expect(TouchEventType type, int x, int y) {
    return expect_contact(type, 0, x, y);
}

int main(void) {
    if (!touch_init() || !expect(TOUCH_EVENT_NONE, 0, 0)) return 1;

    set_sample(1, 0, 100, 200);
    if (!expect(TOUCH_EVENT_DOWN, 200, 699) ||
        !expect(TOUCH_EVENT_NONE, 200, 699)) return 1;
    set_sample(1, 2, 101, 220);
    if (!expect(TOUCH_EVENT_MOVE, 220, 698)) return 1;

    read_ok = 0;
    if (!expect(TOUCH_EVENT_NONE, 220, 698)) return 1;
    read_ok = 1;
    set_sample(11, 0, 0, 0);
    if (!expect(TOUCH_EVENT_NONE, 220, 698)) return 1;
    set_sample(1, 3, 0, 0);
    if (!expect(TOUCH_EVENT_NONE, 220, 698)) return 1;
    set_sample(0, 0, 0, 0);
    if (!expect(TOUCH_EVENT_NONE, 220, 698) ||
        !expect(TOUCH_EVENT_UP, 220, 698) ||
        !expect(TOUCH_EVENT_NONE, 220, 698)) return 1;

    set_sample(1, 2, 0, 0);
    if (!expect(TOUCH_EVENT_DOWN, 0, 799)) return 1;
    set_sample(1, 1, 0, 0);
    if (!expect(TOUCH_EVENT_UP, 0, 799)) return 1;

    set_sample(2, 0, 0, 0);
    set_contact(0, 7, 0, 100, 80);
    set_contact(1, 9, 0, 150, 300);
    if (!expect_contact(TOUCH_EVENT_DOWN, 7, 80, 699) ||
        !expect_contact(TOUCH_EVENT_DOWN, 9, 300, 649)) return 1;
    set_contact(0, 7, 2, 400, 80);
    set_contact(1, 9, 1, 0, 0);
    if (!expect_contact(TOUCH_EVENT_MOVE, 7, 80, 399) ||
        !expect_contact(TOUCH_EVENT_UP, 9, 300, 649)) return 1;
    return 0;
}
