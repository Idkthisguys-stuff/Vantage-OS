#include <vantage/drivers/sdio.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct {
    uint32_t regs[64], gpio[64], clock, bytes, index, block_size, buffer_base, ready;
    unsigned char data[512], fn0[768];
    int owner, mode, writes;
} Model;
static void advance_transfer(Model* m) {
    CHECK(!(m->regs[0x30 / 4] & m->ready));
    if (m->index == m->bytes) m->regs[0x30 / 4] |= 2;
    else if (m->index % m->block_size == 0) m->regs[0x30 / 4] |= m->ready;
}
static uint32_t rd(void* context, uintptr_t address) {
    Model* m = context;
    if (address >= 0xfe200000ULL && address < 0xfe200100ULL) return m->gpio[(address - 0xfe200000ULL) / 4];
    CHECK(address >= 0xfe300000ULL && address <= 0xfe3000fcULL);
    unsigned int offset = (unsigned int)(address - 0xfe300000ULL);
    if (offset == 0xfc) return 2U << 16;
    if (offset == 0x20) {
        uint32_t value = 0;
        for (unsigned int i = 0; i < 4 && m->index < m->bytes; i++)
            value |= (uint32_t)m->data[m->buffer_base + m->index++] << (i * 8);
        advance_transfer(m);
        return value;
    }
    return m->regs[offset / 4];
}
static void wr(void* context, uintptr_t address, uint32_t value) {
    Model* m = context; m->writes++;
    if (address >= 0xfe200000ULL && address < 0xfe200100ULL) {
        CHECK(address == 0xfe20000cULL || address == 0xfe2000ecULL);
        m->gpio[(address - 0xfe200000ULL) / 4] = value; return;
    }
    CHECK(address >= 0xfe300000ULL && address <= 0xfe3000fcULL);
    unsigned int offset = (unsigned int)(address - 0xfe300000ULL);
    if (offset == 0x30) { m->regs[offset / 4] &= ~value; return; }
    if (offset == 0x2c) { m->regs[offset / 4] = (value & ~(7U << 24)) | ((value & 1) ? 2U : 0U); return; }
    if (offset == 0x20) {
        for (unsigned int i = 0; i < 4 && m->index < m->bytes; i++)
            m->data[m->buffer_base + m->index++] = (unsigned char)(value >> (i * 8));
        advance_transfer(m);
        return;
    }
    m->regs[offset / 4] = value;
    if (offset != 0x0c) return;
    unsigned int command = value >> 24;
    uint32_t argument = m->regs[8 / 4], response = 0;
    if (m->mode == 1) return;
    if (command == 5) response = 0xa0300000U;
    if (command == 3) response = 1U << 16;
    if (command == 7) m->regs[0x30 / 4] |= 2;
    if (command == 52) {
        unsigned int address8 = (argument >> 9) & 0x1ffffU;
        CHECK(address8 < sizeof(m->fn0));
        if (argument & 0x80000000U) m->fn0[address8] = (unsigned char)argument;
        if (address8 == 3) m->fn0[3] = m->fn0[2];
        response = m->fn0[address8];
        if (m->mode == 2) response |= 1U << 11;
    }
    if (command == 53) {
        unsigned int function = (argument >> 28) & 7U, count = argument & 511U;
        unsigned int blocks = argument & (1U << 27) ? count : 1;
        CHECK(function == 1 || function == 2);
        unsigned int fbr = function * 256U + 0x10U;
        unsigned int maximum = m->fn0[fbr] | ((unsigned int)m->fn0[fbr + 1] << 8);
        m->block_size = argument & (1U << 27) ? maximum : count ? count : 512;
        if (!(argument & (1U << 27)) && m->block_size > maximum) response = 0x1100;
        CHECK(blocks && m->regs[4 / 4] == ((blocks << 16) | m->block_size));
        CHECK((value & 0x23U) == (blocks > 1 ? 0x22U : 0U));
        m->bytes = blocks * m->block_size;
        m->buffer_base = function == 1 ? (argument >> 9) & 511U : 0;
        CHECK(m->bytes + m->buffer_base <= sizeof(m->data));
        m->index = 0;
        m->ready = argument & 0x80000000U ? 16U : 32U;
        m->regs[0x30 / 4] |= m->ready;
    }
    m->regs[0x10 / 4] = response; m->regs[0x30 / 4] |= 1;
}
static uint32_t time_us(void* context) { Model* m = context; m->clock += 1000; return m->clock; }
static int owner(void* context) { return ((Model*)context)->owner; }
static int property(void* context, uint32_t tag, uint32_t* values, unsigned int count) {
    Model* m = context;
    CHECK(count == 2);
    if (tag == 0x30002) { CHECK(values[0] == 1); values[1] = 100000000; }
    else {
        CHECK(tag == 0x38041 && values[0] == 129);
        if (m->mode == 3) values[0] = 0;
    }
    return HW_OK;
}
int main(void) {
    Model m = {0}; Sdio host = {0}; m.owner = 1;
    HwOps io = {&m, rd, wr, time_us, owner, NULL, NULL, property};
    CHECK(sdio_init(&host, &io) == HW_OK && host.rca == 65536 && host.source_hz == 100000000);
    uint32_t selects = m.gpio[0xc / 4];
    CHECK((selects & 0xfffU) == 0); /* GPIO 30..33 untouched. */
    for (unsigned int pin = 34; pin <= 39; pin++) CHECK(((selects >> ((pin % 10) * 3)) & 7U) == 7);
    unsigned char input[512], output[512];
    for (unsigned int i = 0; i < sizeof(input); i++) input[i] = (unsigned char)i;
    CHECK(sdio_transfer(&host, 1, 0x8000, 1, 1, input, 512) == HW_OK);
    CHECK(m.regs[8 / 4] == 0x9d000008U && !memcmp(input, m.data, sizeof(input)));
    memset(output, 0, sizeof(output));
    CHECK(sdio_transfer(&host, 1, 0x8000, 1, 0, output, 67) == HW_OK);
    CHECK(m.regs[8 / 4] == 0x15008003U && !memcmp(input, output, 67) && output[67] == 0);
    CHECK(sdio_transfer(&host, 2, 0, 0, 1, input, 512) == HW_OK);
    CHECK(!memcmp(input, m.data, sizeof(input)));
    CHECK(sdio_transfer(&host, 2, 0, 0, 0, output, 511) == HW_OK);
    CHECK(!memcmp(input, output, 511));
    CHECK(sdio_transfer(&host, 1, 0x1ffffU, 1, 1, input, 2) == HW_INVALID);
    CHECK(sdio_transfer(&host, 2, 0, 0, 1, input, 513) == HW_INVALID);
    m.owner = 0; int before = m.writes;
    CHECK(sdio_transfer(&host, 2, 0, 0, 1, input, 1) == HW_INVALID && before == m.writes);
    m.owner = 1; m.mode = 2; uint8_t byte = 0;
    CHECK(sdio_direct(&host, 0, 3, 0, &byte) == HW_PROTOCOL && host.state == -1);
    CHECK((host.failure_command >> 24) == 52 && host.failure_argument == (3U << 9));
    CHECK(host.failure_response & (1U << 11));
    memset(&host, 0, sizeof(host)); memset(&m, 0, sizeof(m)); m.owner = 1; m.mode = 3;
    CHECK(sdio_init(&host, &io) == HW_OK);
    memset(&host, 0, sizeof(host)); memset(&m, 0, sizeof(m)); m.owner = 1; m.mode = 1; m.clock = 0xffff0000U;
    CHECK(sdio_init(&host, &io) == HW_TIMEOUT && host.state == -1);
    puts("PASS: SDIO PIO, CMD52/53, tail bytes, R5 errors, timeout, owner and SD-slot exclusion");
    return 0;
}
