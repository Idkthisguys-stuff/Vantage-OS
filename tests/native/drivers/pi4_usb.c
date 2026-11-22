#include <vantage/arch/mmu.h>
#include <vantage/drivers/pi4_usb.h>
#include <stdio.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "USB PCIe probe check failed at line %d\n", __LINE__); return 1; \
} } while (0)

#define HOST 0xfd500000ULL

typedef struct {
    uint32_t regs[0x9214 / 4];
    uint32_t time, released, config_reads, capability_reads, bar_mask;
    int owned, force_down, drop_after_class, invalid_bus, bad_caps, bad_access;
} Model;

static uint32_t read32(void* context, uintptr_t address) {
    Model* model = context;
    if (address >= MMU_PCIE_DEVICE_BASE) {
        model->capability_reads++;
        if (model->regs[0x400c / 4] != 0xf8000000 ||
            model->regs[0x4010 / 4] != 0 ||
            model->regs[0x4070 / 4] != 0x03f00000 ||
            model->regs[0x4080 / 4] != 6 ||
            model->regs[0x4084 / 4] != 6 ||
            model->regs[0x20 / 4] != 0xfbf0f800 ||
            model->regs[0x04 / 4] != 0x402 ||
            model->regs[0x8004 / 4] != 0x402 ||
            model->regs[0x8010 / 4] != 0xf8000004 ||
            model->regs[0x8014 / 4]) model->bad_access = 1;
        if (model->bad_caps) return 0xffffffff;
        if (address == MMU_PCIE_DEVICE_BASE) return 0x01000020;
        if (address == MMU_PCIE_DEVICE_BASE + 4) return 0x06000020;
    }
    if (address < HOST || address >= HOST + sizeof(model->regs) || (address & 3)) {
        model->bad_access = 1;
        return 0xffffffff;
    }
    unsigned int offset = (unsigned int)(address - HOST);
    if (offset == 0x4068)
        return 0x80U | (!model->force_down && model->released ? 0x30U : 0U);
    if (offset >= 0x8000 && offset < 0x9000) {
        model->config_reads++;
        if (model->force_down || !model->released || model->time - model->released < 100000 ||
            model->regs[0x9000 / 4] != 0x100000 ||
            (model->regs[0x18 / 4] & 0xffffffU) != 0x010100) model->bad_access = 1;
        if (offset == 0x8000 && model->invalid_bus) return 0xffffffff;
        if (offset == 0x8008 && model->drop_after_class) model->force_down = 1;
        if (offset == 0x8010 && model->regs[offset / 4] == 0xffffffff)
            return model->bar_mask | 4;
        if (offset == 0x8010) return model->regs[offset / 4] | 4;
    }
    return model->regs[offset / 4];
}

static void write32(void* context, uintptr_t address, uint32_t value) {
    Model* model = context;
    if (address < HOST || address >= HOST + sizeof(model->regs) || (address & 3)) {
        model->bad_access = 1;
        return;
    }
    unsigned int offset = (unsigned int)(address - HOST);
    if (offset == 0x9210) {
        if ((model->regs[offset / 4] & 1) && !(value & 1)) model->released = model->time;
        if (value & 1) model->released = 0;
    }
    if ((offset == 0x4 || offset == 0x8004) && (value & 4)) model->bad_access = 1;
    if (offset >= 0x8000 && offset < 0x9000 && model->force_down) model->bad_access = 1;
    if ((offset == 0x8010 || offset == 0x8014) && (model->regs[0x8004 / 4] & 2))
        model->bad_access = 1;
    model->regs[offset / 4] = offset == 0x8010 ? value | 4 : value;
}

static uint32_t clock_us(void* context) {
    Model* model = context;
    return model->time += 50;
}

static int owned(void* context) { return ((Model*)context)->owned; }

static void reset_model(Model* model) {
    memset(model, 0, sizeof(*model));
    model->owned = 1;
    model->bar_mask = 0xfffff000;
    model->regs[0] = 0x271114e4;
    model->regs[0x8000 / 4] = 0x34831106;
    model->regs[0x8008 / 4] = 0x0c033001;
    model->regs[0x4034 / 4] = 0x11;  /* Inbound window left by firmware. */
}

int main(void) {
    Model model;
    HwOps io = {&model, read32, write32, clock_us, owned, NULL, NULL, NULL};
    Pi4UsbProbe probe;
    reset_model(&model);
    model.owned = 0;
    CHECK(pi4_usb_probe(&io, &probe) == HW_INVALID && probe.stage == PI4_USB_BRIDGE);
    CHECK(!model.time && !model.regs[0x9210 / 4]);

    reset_model(&model);
    model.force_down = 1;
    CHECK(pi4_usb_probe(&io, &probe) == HW_TIMEOUT && probe.stage == PI4_USB_LINK);
    CHECK(model.regs[0x9210 / 4] == 1 && !model.config_reads && !model.capability_reads);

    reset_model(&model);
    model.invalid_bus = 1;
    CHECK(pi4_usb_probe(&io, &probe) == HW_UNSUPPORTED && probe.stage == PI4_USB_ENDPOINT);
    CHECK(!model.capability_reads && !model.bad_access);

    reset_model(&model);
    model.drop_after_class = 1;
    CHECK(pi4_usb_probe(&io, &probe) == HW_TIMEOUT && probe.stage == PI4_USB_BAR);
    CHECK(model.config_reads == 2 && !model.capability_reads && !model.bad_access);

    reset_model(&model);
    model.bar_mask = 0xf8000000;
    CHECK(pi4_usb_probe(&io, &probe) == HW_PROTOCOL && probe.stage == PI4_USB_BAR);
    CHECK(!model.capability_reads && !model.bad_access);

    reset_model(&model);
    model.bad_caps = 1;
    CHECK(pi4_usb_probe(&io, &probe) == HW_PROTOCOL && probe.stage == PI4_USB_XHCI);
    CHECK(model.capability_reads == 1 && !model.bad_access && model.regs[0x9210 / 4] == 1);

    reset_model(&model);
    CHECK(pi4_usb_probe(&io, &probe) == HW_OK && probe.stage == PI4_USB_READY);
    CHECK(probe.version == 0x100 && probe.ports == 6 && !model.bad_access);
    CHECK(model.capability_reads == 2 && model.released);
    CHECK(!model.regs[0x4034 / 4]);
    return 0;
}
