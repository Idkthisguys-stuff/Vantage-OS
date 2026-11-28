/* CYW43455 SDIO/backplane and SDPCM/BCDC protocol, referenced against
 * Linux v6.12 brcmfmac (ISC, Copyright 2010/2014 Broadcom Corporation).
 * See docs/licenses/brcmfmac-ISC.txt and docs/HARDWARE.md. */
#include <vantage/drivers/wifi.h>
#include <vantage/platform/pi4/io.h>

#define RAM_BASE 0x198000U
#define ENUM_BASE 0x18000000U
#define EROM_BYTES 4096U
#define CLOCK_CSR 0x1000eU
enum {
    SDPCM_VERSION = 4, SDIO_FRAME_INDICATION = 0x40, SDIO_HOST_MAILBOX = 0x80,
    MAILBOX_DEVICE_READY = 0x02, MAILBOX_FIRMWARE_READY = 0x08,
    MAILBOX_FIRMWARE_HALTED = 0x10
};

static uint32_t le32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static unsigned int le16(const unsigned char* p) { return (unsigned int)p[0] | ((unsigned int)p[1] << 8); }
static uint32_t be32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put16(unsigned char* p, unsigned int value) { p[0] = (unsigned char)value; p[1] = (unsigned char)(value >> 8); }
static void put32(unsigned char* p, uint32_t value) {
    for (unsigned int i = 0; i < 4; i++) p[i] = (unsigned char)(value >> (i * 8));
}
static void erase(void* memory, size_t bytes) {
    volatile unsigned char* p = memory;
    while (bytes--) *p++ = 0;
}
static const char* fail_where;
static unsigned int dbg_frames, dbg_ctl, dbg_ctl_id, dbg_ctl_cmd, dbg_events,
                    dbg_credit, dbg_idle, dbg_zero, dbg_int;
static int fail(Wifi* w, int result) {
    uart_writeText("[DBG] wifi fail where=");
    uart_writeText(fail_where ? fail_where : "-");
    uart_writeText(" res=");
    uart_writeHex((uint32_t)result);
    uart_writeText(" cmd=");
    uart_writeHex(w->control_command);
    uart_writeText(" set=");
    uart_writeHex((uint32_t)w->control_set);
    uart_writeText(" stage=");
    uart_writeText(w->boot_stage ? w->boot_stage : "none");
    uart_writeText(" id="); uart_writeHex(w->control_id);
    uart_writeText(" tx="); uart_writeHex(w->tx_sequence);
    uart_writeText(" win="); uart_writeHex(w->tx_window);
    uart_writeText(" rx="); uart_writeHex(w->rx_sequence);
    uart_writeText("\n");

    w->state = -1; w->error = result; w->link = w->keyed = w->connected = 0;
    erase(w->tx, sizeof(w->tx)); erase(w->control, sizeof(w->control));
    erase(w->rx, sizeof(w->rx)); erase(w->nvram, sizeof(w->nvram));
    return result;
}
static int reg8(Wifi* w, unsigned int fn, uint32_t address, int write, uint8_t* value) {
    return w->bus.direct(w->bus.context, fn, address, write, value);
}
static int set8(Wifi* w, unsigned int fn, uint32_t address, uint8_t value) { return reg8(w, fn, address, 1, &value); }
static int wait8(Wifi* w, unsigned int fn, uint32_t address, uint8_t mask, uint8_t expected, uint32_t us) {
    uint32_t start = w->bus.time_us(w->bus.context);
    for (unsigned int i = 0; i < 100000; i++) {
        uint8_t value;
        int result = reg8(w, fn, address, 0, &value);
        if (result != HW_OK) return result;
        if ((value & mask) == expected) return HW_OK;
        if (w->bus.time_us(w->bus.context) - start >= us) break;
    }
    return HW_TIMEOUT;
}

static int window(Wifi* w, uint32_t address) {
    address &= ~0x7fffU;
    if (w->window == address) return HW_OK;
    for (unsigned int i = 0; i < 3; i++) {
        int result = set8(w, 1, 0x1000a + i, (uint8_t)(address >> (8 * (i + 1))));
        if (result != HW_OK) return result;
    }
    w->window = address;
    return HW_OK;
}

static int backplane(Wifi* w, uint32_t address, void* data, unsigned int bytes, int write) {
    if (!bytes || address > 0xffffffffU - bytes) return HW_INVALID;
    unsigned char* p = data;
    while (bytes) {
        unsigned int count = 0x8000U - (address & 0x7fffU);
        if (count > bytes) count = bytes;
        if (count > 512) count = 512;
        int result = window(w, address);
        if (result != HW_OK) {
            w->backplane_failure_address = address;
            w->backplane_failure = result;
            return result;
        }
        if (count >= 4) {
            count &= ~3U;
            result = w->bus.transfer(w->bus.context, 1, (address & 0x7fffU) | 0x8000U, 1, write, p, count);
        } else {
            for (unsigned int i = 0; i < count && result == HW_OK; i++)
                result = reg8(w, 1, ((address + i) & 0x7fffU) | 0x8000U, write, p + i);
        }
        if (result != HW_OK) {
            w->backplane_failure_address = address;
            w->backplane_failure = result;
            return result;
        }
        address += count; p += count; bytes -= count;
    }
    return HW_OK;
}
static int read32(Wifi* w, uint32_t address, uint32_t* value) {
    unsigned char data[4];
    int result = backplane(w, address, data, 4, 0);
    if (result == HW_OK) *value = le32(data);
    return result;
}
static int write32(Wifi* w, uint32_t address, uint32_t value) {
    unsigned char data[4]; put32(data, value);
    return backplane(w, address, data, 4, 1);
}
static int wait32(Wifi* w, uint32_t address, uint32_t mask, uint32_t expected) {
    uint32_t start = w->bus.time_us(w->bus.context);
    for (unsigned int i = 0; i < 10000; i++) {
        uint32_t value;
        int result = read32(w, address, &value);
        if (result != HW_OK) return result;
        if ((value & mask) == expected) return HW_OK;
        if (w->bus.time_us(w->bus.context) - start >= 500000U) break;
    }
    return HW_TIMEOUT;
}

enum { CORE_IO = 0x408, CORE_RESET = 0x800, CORE_RESET_ASSERTED = 1,
       CORE_CLOCK = 1, CORE_FORCE_CLOCK = 2 };

static int core_reset_request(Wifi* w, uint32_t address, uint32_t asserted) {
    int result = write32(w, address, asserted);
    if (result != HW_OK) return result;
    return wait32(w, address, CORE_RESET_ASSERTED, asserted);
}

static int core_disable(Wifi* w, uint32_t wrap, uint32_t before, uint32_t after) {
    const uint32_t clocks = CORE_CLOCK | CORE_FORCE_CLOCK;
    const uint32_t reset_reg = wrap + CORE_RESET;
    const uint32_t io_reg = wrap + CORE_IO;
    uint32_t state;
    int result = read32(w, reset_reg, &state);
    if (result != HW_OK) return result;
    if (state & CORE_RESET_ASSERTED) return write32(w, io_reg, clocks | after);

    /* Keep the core clocked while requesting a reset. Reading IO control
     * drains the preceding write on the backplane. */
    result = write32(w, io_reg, clocks | before);
    if (result == HW_OK) result = read32(w, io_reg, &state);
    if (result == HW_OK) result = core_reset_request(w, reset_reg, CORE_RESET_ASSERTED);
    if (result != HW_OK) return result;
    return write32(w, io_reg, clocks | after);
}
static int core_reset(Wifi* w, uint32_t wrap, uint32_t before, uint32_t after) {
    int result = core_disable(w, wrap, before, after);
    if (result == HW_OK) result = core_reset_request(w, wrap + CORE_RESET, 0);
    if (result == HW_OK) result = w->bus.delay(w->bus.context, 40);
    if (result != HW_OK) return result;
    return write32(w, wrap + CORE_IO, CORE_CLOCK | after);
}

static int erom_word(Wifi* w, uint32_t* value) {
    if (w->erom_offset >= EROM_BYTES) return HW_PROTOCOL;
    int result = read32(w, w->erom_base + w->erom_offset, value);
    if (result == HW_OK) w->erom_offset += 4;
    return result;
}

static int chip_probe(Wifi* w) {
    uint32_t value;
    w->boot_stage = "chip ID";
    int result = read32(w, ENUM_BASE, &value);
    if (result != HW_OK) return result;
    w->chip = value & 0xffffU; w->revision = (value >> 16) & 15U;
    if (w->chip != 0x4345 || w->revision != 6 || (value >> 28) != 1) return HW_UNSUPPORTED;

    w->boot_stage = "core enumeration";
    result = read32(w, ENUM_BASE + 0xfc, &w->erom_base);
    if (result != HW_OK) return result;
    uint32_t erom = w->erom_base;
    if ((erom & 3) || erom < ENUM_BASE || erom > 0x18200000U - EROM_BYTES) return HW_PROTOCOL;
    unsigned int core = 0, core_revision = 0, ended = 0;
    /* The enumeration ROM terminates at END. Do not read later words. */
    while (w->erom_offset < EROM_BYTES) {
        result = erom_word(w, &value);
        if (result != HW_OK) return result;
        unsigned int type = value & 15U;
        if (type == 15) { ended = 1; break; }
        if (!(value & 1)) continue;
        if (type == 1) {
            core = (value >> 8) & 0xfffU;
            uint32_t descriptor;
            result = erom_word(w, &descriptor);
            if (result != HW_OK) return result;
            if ((descriptor & 15U) != 1) return HW_PROTOCOL;
            core_revision = descriptor >> 24;
        } else if ((type & ~8U) == 5) {
            uint32_t address_high = 0;
            if (type & 8) {
                result = erom_word(w, &address_high);
                if (result != HW_OK) return result;
            }
            unsigned int size_type = (value >> 4) & 3U;
            if (size_type == 3) {
                uint32_t descriptor;
                result = erom_word(w, &descriptor);
                if (result != HW_OK) return result;
                if (descriptor & 8U) {
                    result = erom_word(w, &descriptor);
                    if (result != HW_OK) return result;
                }
            }
            /* A region outside our backplane window can coexist with the
             * required registers. Consume its full descriptor before skipping. */
            if (address_high || size_type > 1) continue;
            uint32_t address = value & 0xfffff000U;
            if (address < ENUM_BASE || address >= 0x18200000U) continue;
            unsigned int kind = (value >> 6) & 3U;
            if (core == 0x83e && kind == 0 && !w->arm) w->arm = address;
            if (core == 0x83e && kind >= 2 && !w->arm_wrap) w->arm_wrap = address;
            if (core == 0x812 && kind >= 2 && !w->d11_wrap) w->d11_wrap = address;
            if (core == 0x829 && kind == 0 && !w->sdregs) { w->sdregs = address; w->sd_revision = core_revision; }
        }
    }
    if (!ended || !w->arm || !w->arm_wrap || !w->d11_wrap || !w->sdregs) return HW_PROTOCOL;
    w->boot_stage = "core reset";
    if (core_reset(w, w->arm_wrap, 0x20, 0x20) != HW_OK ||
        core_disable(w, w->d11_wrap, 12, 4) != HW_OK) return HW_IO;
    w->boot_stage = "RAM discovery";
    if (read32(w, w->arm + 4, &value) != HW_OK) return HW_IO;
    unsigned int banks = (value & 15U) + ((value >> 4) & 15U);
    if (!banks || banks > 16) return HW_UNSUPPORTED;
    for (unsigned int i = 0; i < banks; i++) {
        if (write32(w, w->arm + 0x40, i) != HW_OK || read32(w, w->arm + 0x44, &value) != HW_OK) return HW_IO;
        w->ram_bytes += ((value & 127U) + 1) * ((value & 512U) ? 1024U : 8192U);
    }
    if (w->ram_bytes < 0x20000 || w->ram_bytes > 0x200000) return HW_UNSUPPORTED;
    return HW_OK;
}

static int fifo(Wifi* w, int write, unsigned char* data, unsigned int bytes) {
    /* Function 2 exposes the frame FIFO through the ChipCommon window. */
    int result = window(w, ENUM_BASE);
    if (result != HW_OK) return result;
    while (bytes) {
        unsigned int count = bytes > 512 ? 512 : bytes;
        result = w->bus.transfer(w->bus.context, 2, 0x8000U, 0, write, data, count);
        if (result != HW_OK) return result;
        data += count; bytes -= count;
    }
    return HW_OK;
}

/* RSN/WPA information elements decide what the station can offer. */
static uint8_t security(unsigned int capability, const unsigned char* ie, uint32_t bytes) {
    int rsn = 0, wpa = 0, psk = 0, sae = 0, dot1x = 0, other = 0;
    for (uint32_t offset = 0; offset + 2 <= bytes;) {
        unsigned int id = ie[offset], length = ie[offset + 1];
        if (length > bytes - offset - 2) return WIFI_OTHER;  /* Truncated element list. */
        const unsigned char* body = ie + offset + 2;
        if (id == 48 && length >= 8) {
            rsn = 1;
            unsigned int pairwise = le16(body + 6), position = 8 + pairwise * 4;
            if (pairwise > 16 || position + 2 > length) { other = 1; goto next; }
            unsigned int suites = le16(body + position);
            position += 2;
            if (suites > 16 || position + suites * 4 > length) { other = 1; goto next; }
            for (unsigned int i = 0; i < suites; i++, position += 4) {
                const unsigned char* suite = body + position;
                if (suite[0] != 0 || suite[1] != 0x0f || suite[2] != 0xac) { other = 1; continue; }
                if (suite[3] == 2 || suite[3] == 6) psk = 1;
                else if (suite[3] == 8) sae = 1;
                else if (suite[3] == 1 || suite[3] == 5) dot1x = 1;
                else other = 1;
            }
        } else if (id == 221 && length >= 4 && body[0] == 0 && body[1] == 0x50 && body[2] == 0xf2 && body[3] == 1) {
            wpa = 1;
        }
    next:
        offset += 2 + length;
    }
    if (rsn) {
        if (psk && sae) return WIFI_WPA2_WPA3;
        if (psk) return WIFI_WPA2;
        if (sae) return WIFI_WPA3;
        return dot1x && !other ? WIFI_ENTERPRISE : WIFI_OTHER;
    }
    if (wpa) return WIFI_WPA;
    return (capability & 0x10U) ? WIFI_WEP : WIFI_OPEN;
}

/* One brcmf_escan_result carrying exactly one brcmf_bss_info record. */
static void scan_event(Wifi* w, uint32_t status, const unsigned char* d, uint32_t bytes) {
    if (status != 8) { w->scanning = 0; w->scanned = 1; return; }
    if (!w->scanning || bytes < 12 + 128) return;
    uint32_t buffer = le32(d);
    if (buffer < 12 + 128 || buffer > bytes || le16(d + 10) != 1) return;
    const unsigned char* b = d + 12;
    uint32_t length = le32(b + 4), ie_length = le32(b + 120);
    unsigned int ie_offset = le16(b + 116), ssid_bytes = b[18];
    if (length != buffer - 12 || length < 128 || ie_offset < 128 || ie_offset > length ||
        ie_length > length - ie_offset || !ssid_bytes || ssid_bytes > 32) return;
    unsigned int zero = 1;
    for (unsigned int i = 0; i < ssid_bytes; i++) zero &= !b[19 + i];
    if (zero) return; /* Hidden networks cannot be joined by name. */
    WifiBss bss = {0};
    memcpy(bss.bssid, b + 8, 6);
    bss.ssid_bytes = (uint8_t)ssid_bytes;
    memcpy(bss.ssid, b + 19, ssid_bytes);
    bss.channel = b[88] ? b[88] : (uint8_t)le16(b + 72);
    bss.rssi = (int16_t)le16(b + 78);
    bss.security = security(le16(b + 16), b + ie_offset, ie_length);
    unsigned int slot = w->bss_count, weakest = 0;
    for (unsigned int i = 0; i < w->bss_count; i++) {
        if (!memcmp(w->bss[i].bssid, bss.bssid, 6)) { slot = i; break; }
        if (w->bss[i].rssi < w->bss[weakest].rssi) weakest = i;
    }
    if (slot == WIFI_SCAN_LIMIT) {
        if (bss.rssi <= w->bss[weakest].rssi) return;
        slot = weakest;
    }
    if (slot == w->bss_count) w->bss_count++;
    w->bss[slot] = bss;
}

static void event(Wifi* w, const unsigned char* p, unsigned int bytes) {
    /* Ethernet + Broadcom vendor header + versioned big-endian wl_event_msg. */
    if (bytes < 72 || p[12] != 0x88 || p[13] != 0x6c || p[19] != 0 || p[20] != 0x10 || p[21] != 0x18 ||
        p[22] != 0 || p[23] != 1 || p[24] != 0 || p[25] != 2 || p[70] || p[71]) return;
    const unsigned char* message = p + 24;
    uint32_t type = be32(message + 4), status = be32(message + 8), length = be32(message + 20);
    if (length > bytes - 72) return;
    if (type == 16) {
        w->link = !!(message[3] & 1U);
        if (!w->link) { w->keyed = w->connected = 0; w->rx_count = w->rx_head = 0; }
    } else if (type == 46) {
        if (status == 6) w->keyed = 1;
        else if (status == 7) { w->keyed = w->connected = 0; w->join_error = 1; }
    } else if (type == 0) {
        /* SET_SSID status 3 (BRCMF_E_STATUS_NO_NETWORKS): nothing answered. */
        if (status) { w->join_error = 1; w->connected = 0; w->join_missing = status == 3; }
        else w->associated = 1;
    } else if (type == 69) scan_event(w, status, p + 72, length);
    else if (type == 5 || type == 6 || type == 11 || type == 12) {
        w->link = w->keyed = w->connected = 0; w->rx_count = w->rx_head = 0;
        if (w->joining) w->join_error = 1;
    }
}

static int reject_frame(Wifi* w, const char* why) {
    w->boot_frame_error = why;
    for (unsigned int i = 0; i < 3; i++) w->boot_frame_header[i] = le32(w->rx + 4 * i);
    return HW_PROTOCOL;
}

static int pump(Wifi* w) {
    uint32_t interrupts, mailbox;
    if (read32(w, w->sdregs + 0x20, &interrupts) != HW_OK) return HW_IO;
    if (w->state == 2) {
        w->boot_observed = 1;
        w->boot_interrupts |= interrupts;
    }
    w->host_flow = !!(interrupts & 16U);
    dbg_int |= interrupts;
    if (interrupts && write32(w, w->sdregs + 0x20, interrupts) != HW_OK) return HW_IO;
    if (interrupts & SDIO_HOST_MAILBOX) {
        if (read32(w, w->sdregs + 0x4c, &mailbox) != HW_OK || write32(w, w->sdregs + 0x40, 2) != HW_OK) return HW_IO;
        if (w->state == 2) w->boot_mailbox = mailbox;
        if (mailbox & MAILBOX_FIRMWARE_HALTED) return HW_IO;
        if (mailbox & (MAILBOX_DEVICE_READY | MAILBOX_FIRMWARE_READY)) {
            if (((mailbox >> 16) & 255U) != SDPCM_VERSION) return HW_UNSUPPORTED;
            w->protocol_ready = 1;
        }
    }
    
    int read_ahead = w->rx_next_length != 0;

    if (read_ahead) {
        w->rx_next_length = 0;
    } else if (!(interrupts & SDIO_FRAME_INDICATION)) {
        uint8_t low, high;

        if (reg8(w, 1, 0x1001b, 0, &low) != HW_OK ||
            reg8(w, 1, 0x1001c, 0, &high) != HW_OK)
            return HW_IO;

        if (!low && !high) {
            dbg_idle++;
            return 0;
        }
    }
    if (fifo(w, 0, w->rx, 12) != HW_OK) return HW_IO;
    unsigned int length = le16(w->rx), complement = le16(w->rx + 2);

    uart_writeText("[DBG] SDPCM channel=");
    uart_writeHex(w->rx[5] & 0x0F);

    uart_writeText(" sequence=");
    uart_writeHex(w->rx[4]);

    uart_writeText(" nextlen=");
    uart_writeHex(w->rx[6]);

    uart_writeText(" frame_length=");
    uart_writeHex(length);

    uart_writeText("\n");

    if (!length && !complement) { dbg_zero++; return 0; }
    if ((length ^ complement) != 0xffffU || length < 12 || length > sizeof(w->rx))
        return reject_frame(w, "invalid size/checksum");
    if (length > 12 && (w->rx[7] < 12 || w->rx[7] > length))
        return reject_frame(w, "invalid payload offset");
    if (w->rx_synced && w->rx[4] != w->rx_sequence) return reject_frame(w, "unexpected receive sequence");
    if (length > 12 && fifo(w, 0, w->rx + 12, (length - 12 + 3U) & ~3U) != HW_OK) return HW_IO;
    w->rx_synced = 1;
    w->rx_sequence = (uint8_t)(w->rx[4] + 1U);
    w->rx_next_length = w->rx[6];
    /* Keep the last usable credit when the firmware advertises a stale window. */
    if ((uint8_t)(w->rx[9] - w->tx_sequence) <= 64U) w->tx_window = w->rx[9];
    w->flow_mask = w->rx[8];
    dbg_frames++;
    if (length == 12) { dbg_credit++; return HW_OK; } /* Credit update with no payload. */
    unsigned int channel = w->rx[5] & 15U, offset = w->rx[7];
    unsigned char* p = w->rx + offset;
    unsigned int bytes = length - offset;
    if (channel == 0) {
        if (bytes < 16) return reject_frame(w, "short control frame");
        dbg_ctl++; dbg_ctl_id = le32(p + 8) >> 16; dbg_ctl_cmd = le32(p);
        if ((le32(p + 8) >> 16) != (w->control_id & 65535U)) return HW_OK;
        if (le32(p) != w->control_command) return reject_frame(w, "unexpected control command");
        w->control_status = le32(p + 12) | (le32(p + 8) & 1U);
        w->control_length = w->control_set || w->control_status ? 0 : le32(p + 4);
        if (w->control_length > bytes - 16 || w->control_length > sizeof(w->control))
            return reject_frame(w, "invalid control length");
        memcpy(w->control, p + 16, w->control_length);
        w->control_ready = 1;
    } else if (channel == 1 || channel == 2) {
        if (bytes < 4 || (p[0] >> 4) != 2 || (p[2] & 15U) != 0)
            return reject_frame(w, "invalid network header");
        unsigned int skip = 4U + (unsigned int)p[3] * 4U;
        if (skip > bytes) return reject_frame(w, "invalid network offset");
        p += skip; bytes -= skip;
        if (channel == 1) { dbg_events++; event(w, p, bytes); }
        else if (bytes >= 14 && bytes <= WIFI_FRAME_MAX && w->connected && w->link && (w->keyed || w->open)) {
            if (w->rx_count == 4) w->dropped++;
            else {
                unsigned int slot = (w->rx_head + w->rx_count) % 4;
                memcpy(w->frames[slot], p, bytes); w->frame_bytes[slot] = bytes; w->rx_count++;
            }
        }
    } else return HW_UNSUPPORTED; /* Glom aggregation is disabled during setup. */
    return HW_OK;
}

static int credit(Wifi* w, unsigned int channel) {
    uint32_t start = w->bus.time_us(w->bus.context);
    for (unsigned int i = 0; i < 100000; i++) {
        if (w->tx_sequence != w->tx_window && (!channel || (!w->host_flow && !(w->flow_mask & 1U)))) return HW_OK;
        int result = pump(w);
        if (result < 0) return result;
        if (w->bus.time_us(w->bus.context) - start >= 2500000U) break;
    }
    return HW_TIMEOUT;
}

static void header(Wifi* w, unsigned int bytes, unsigned int channel) {
    put16(w->tx, bytes); put16(w->tx + 2, bytes ^ 65535U);
    w->tx[4] = w->tx_sequence; w->tx[5] = (unsigned char)channel; w->tx[7] = 12;
}

static void dbg_timeout(Wifi* w) {
    uint8_t lo = 0, hi = 0;
    uint32_t ints = 0, mbox = 0;
    (void)reg8(w, 1, 0x1001b, 0, &lo); (void)reg8(w, 1, 0x1001c, 0, &hi);
    (void)read32(w, w->sdregs + 0x20, &ints); (void)read32(w, w->sdregs + 0x4c, &mbox);
    uart_writeText("[DBG] reply wait: frames="); uart_writeHex(dbg_frames);
    uart_writeText(" ctl="); uart_writeHex(dbg_ctl);
    uart_writeText(" last_ctl_id="); uart_writeHex(dbg_ctl_id);
    uart_writeText(" last_ctl_cmd="); uart_writeHex(dbg_ctl_cmd);
    uart_writeText(" want_id="); uart_writeHex(w->control_id);
    uart_writeText(" events="); uart_writeHex(dbg_events);
    uart_writeText(" credit="); uart_writeHex(dbg_credit);
    uart_writeText(" idle="); uart_writeHex(dbg_idle);
    uart_writeText(" zero="); uart_writeHex(dbg_zero);
    uart_writeText(" seen_int="); uart_writeHex(dbg_int);
    uart_writeText(" int_now="); uart_writeHex(ints);
    uart_writeText(" pending="); uart_writeHex(lo); uart_writeText(" "); uart_writeHex(hi);
    uart_writeText(" mbox="); uart_writeHex(mbox);
    uart_writeText("\n");
}

static int control(Wifi* w, uint32_t command, int set, const void* input, unsigned int input_bytes,
                    void* output, unsigned int capacity, unsigned int* output_bytes) {
    unsigned int payload = input_bytes > capacity ? input_bytes : capacity;
    if (payload > sizeof(w->control) || (input_bytes && !input) || (capacity && !output)) return HW_INVALID;
    w->control_status = 0;
    fail_where = "credit";
    int result = credit(w, 0);
    if (result != HW_OK) return fail(w, result);
    memset(w->tx, 0, (payload + 28 + 3U) & ~3U);
    w->control_id = (w->control_id + 1) & 65535U;
    w->control_command = command; w->control_ready = 0; w->control_set = set;
    header(w, payload + 28, 0);
    put32(w->tx + 12, command); put32(w->tx + 16, payload);
    put32(w->tx + 20, (w->control_id << 16) | (set ? 2U : 0U));
    if (input_bytes) memcpy(w->tx + 28, input, input_bytes);
    if (set && command == 26) w->link = w->keyed = w->associated = w->join_error = 0;
    fail_where = "send";
    result = fifo(w, 1, w->tx, (payload + 28 + 3U) & ~3U);
    erase(w->tx, sizeof(w->tx));
    if (result != HW_OK) return fail(w, result);
    w->tx_sequence++;
    fail_where = "reply";
    dbg_frames = dbg_ctl = dbg_ctl_id = dbg_ctl_cmd = dbg_events = 0;
    dbg_credit = dbg_idle = dbg_zero = dbg_int = 0;
    uint32_t start = w->bus.time_us(w->bus.context);
    for (unsigned int i = 0; !w->control_ready && i < 100000; i++) {
        result = pump(w);
        if (result < 0) return fail(w, result);
        if (w->bus.time_us(w->bus.context) - start >= 2500000U) { dbg_timeout(w); return fail(w, HW_TIMEOUT); }
    }
    if (!w->control_ready) { dbg_timeout(w); return fail(w, HW_TIMEOUT); }
    erase(w->rx, sizeof(w->rx));
    if (w->control_status) { erase(w->control, sizeof(w->control)); return HW_IO; }
    if (!set && w->control_length > capacity) {
        if (command != 262 || w->control_length != payload) {
            erase(w->control, sizeof(w->control));
            return HW_NO_SPACE;
        }
        w->control_length = capacity;
    }
    if (!set && w->control_length) memcpy(output, w->control, w->control_length);
    if (output_bytes) *output_bytes = set ? 0 : w->control_length;
    fail_where = NULL;
    erase(w->control, sizeof(w->control));
    return HW_OK;
}

static int variable(Wifi* w, const char* name, int set, const void* value, unsigned int bytes,
                     void* output, unsigned int capacity, unsigned int* actual) {
    unsigned char request[1536];
    unsigned int name_bytes = 0;
    while (name_bytes < 63 && name[name_bytes]) name_bytes++;
    if (name_bytes == 63 || name_bytes + 1 + bytes > sizeof(request)) return HW_INVALID;
    memcpy(request, name, name_bytes + 1);
    if (bytes) memcpy(request + name_bytes + 1, value, bytes);
    int result = control(w, set ? 263U : 262U, set, request, name_bytes + 1 + bytes, output, capacity, actual);
    erase(request, sizeof(request));
    return result;
}
static int var_int(Wifi* w, const char* name, uint32_t value) {
    unsigned char data[4]; put32(data, value);
    return variable(w, name, 1, data, 4, NULL, 0, NULL);
}
static int command_int(Wifi* w, uint32_t command, uint32_t value) {
    unsigned char data[4]; put32(data, value);
    return control(w, command, 1, data, 4, NULL, 0, NULL);
}

int wifi_boot(Wifi* w, const WifiBus* bus, const WifiFirmware* f) {
    return wifi_boot_traced(w, bus, f, NULL);
}

int wifi_boot_traced(Wifi* w, const WifiBus* bus, const WifiFirmware* f, WifiBootTrace trace) {
    if (!w || !bus || !bus->direct || !bus->transfer || !bus->time_us || !bus->delay || !bus->owned ||
        !bus->owned(bus->context) || !f || !f->firmware || f->firmware_bytes < 4 || f->firmware_bytes > 1048576 ||
        !f->clm || !f->clm_bytes || f->clm_bytes > 262144 || f->country[2] ||
        f->country[0] < 'A' || f->country[0] > 'Z' || f->country[1] < 'A' || f->country[1] > 'Z' ||
        f->country_revision < -1) return HW_INVALID;
    if (w->state) return HW_BUSY;
    w->bus = *bus; w->state = 2; w->window = ~0U; w->tx_window = 1; w->secure = 1;
    size_t nvram_bytes = 0;
    if (wifi_nvram_pack(f->nvram, f->nvram_bytes, w->nvram, sizeof(w->nvram), &nvram_bytes) != HW_OK)
        return fail(w, HW_INVALID);
    w->boot_stage = "backplane clock";
    if (set8(w, 1, CLOCK_CSR, 0x28) != HW_OK || wait8(w, 1, CLOCK_CSR, 64, 64, 500000) != HW_OK ||
        set8(w, 1, CLOCK_CSR, 0x21) != HW_OK || bus->delay(bus->context, 65) != HW_OK) return fail(w, HW_TIMEOUT);
    int result = chip_probe(w);
    if (result != HW_OK) return fail(w, result);
    w->boot_stage = "firmware upload";
    if (w->sd_revision >= 12) {
        uint8_t sleep;
        if (reg8(w, 1, 0x1001f, 0, &sleep) != HW_OK || set8(w, 1, 0x1001f, sleep | 1U) != HW_OK)
            return fail(w, HW_IO);
    }
    if (f->firmware_bytes + nvram_bytes + 4 > w->ram_bytes) return fail(w, HW_NO_SPACE);
    if (write32(w, RAM_BASE + w->ram_bytes - 4, 0) != HW_OK ||
        backplane(w, RAM_BASE, (void*)f->firmware, (unsigned int)f->firmware_bytes, 1) != HW_OK)
        return fail(w, HW_IO);
    w->boot_stage = "firmware readback";
    /* Read back every downloaded byte before releasing the radio CPU. */
    for (unsigned int offset = 0; offset < f->firmware_bytes; offset += 512) {
        unsigned int count = (unsigned int)f->firmware_bytes - offset;
        if (count > 512) count = 512;
        if (backplane(w, RAM_BASE + offset, w->rx, count, 0) != HW_OK ||
            memcmp(w->rx, f->firmware + offset, count)) return fail(w, HW_IO);
    }
    w->boot_stage = "NVRAM upload and readback";
    uint32_t nvram_start = RAM_BASE + w->ram_bytes - 4 - (uint32_t)nvram_bytes;
    uint32_t words = (uint32_t)nvram_bytes / 4;
    if (backplane(w, nvram_start, w->nvram, (unsigned int)nvram_bytes, 1) != HW_OK ||
        write32(w, RAM_BASE + w->ram_bytes - 4, words | ((~words & 65535U) << 16)) != HW_OK)
        return fail(w, HW_IO);
    for (unsigned int offset = 0; offset < nvram_bytes; offset += 512) {
        unsigned int count = (unsigned int)nvram_bytes - offset;
        if (count > 512) count = 512;
        if (backplane(w, nvram_start + offset, w->rx, count, 0) != HW_OK ||
            memcmp(w->rx, w->nvram + offset, count)) return fail(w, HW_IO);
    }
    w->boot_stage = "radio CPU release";
    uint32_t token;
    if (read32(w, RAM_BASE + w->ram_bytes - 4, &token) != HW_OK || token != (words | ((~words & 65535U) << 16)) ||
        write32(w, w->sdregs + 0x20, ~0U) != HW_OK || !le32(f->firmware) ||
        write32(w, 0, le32(f->firmware)) != HW_OK || core_reset(w, w->arm_wrap, 0x20, 0) != HW_OK)
        return fail(w, HW_IO);
    w->boot_stage = "SDIO function 2 enable";
    if (set8(w, 1, CLOCK_CSR, 0x10) != HW_OK || wait8(w, 1, CLOCK_CSR, 128, 128, 1000000) != HW_OK ||
        set8(w, 1, CLOCK_CSR, 0x12) != HW_OK || write32(w, w->sdregs + 0x48, SDPCM_VERSION << 16) != HW_OK ||
        set8(w, 0, 2, 6) != HW_OK || wait8(w, 0, 3, 6, 6, 1000000) != HW_OK ||
        write32(w, w->sdregs + 0x24, 0xf0) != HW_OK || set8(w, 0, 4, 7) != HW_OK ||
        set8(w, 1, 0x10008, 0x60) != HW_OK || set8(w, 1, 0x1001d, 0xd0) != HW_OK)
        return fail(w, HW_IO);
    uint8_t device;
    if (reg8(w, 1, 0x10009, 0, &device) != HW_OK || set8(w, 1, 0x10009, device | 16U) != HW_OK)
        return fail(w, HW_IO);
    w->boot_stage = "SDPCM ready mailbox";
    uint32_t ready_start = bus->time_us(bus->context);
    for (unsigned int attempt = 0; !w->protocol_ready; attempt++) {
        result = pump(w);
        if (result < 0) return fail(w, result);
        if (w->protocol_ready) break;
        if (attempt == 100000 || bus->time_us(bus->context) - ready_start >= 1000000U) {
            w->boot_ram_tail_valid = read32(w, RAM_BASE + w->ram_bytes - 4, &w->boot_ram_tail) == HW_OK;
            return fail(w, HW_TIMEOUT);
        }
    }
    w->boot_stage = "CLM upload";
    for (size_t offset = 0; offset < f->clm_bytes;) {
        unsigned char chunk[1040] = {0};
        unsigned int count = (unsigned int)(f->clm_bytes - offset);
        if (count > 1024) count = 1024;
        unsigned int flags = 0x1000U | (!offset ? 2U : 0U) | (offset + count == f->clm_bytes ? 4U : 0U);
        put16(chunk, flags); put16(chunk + 2, 2); put32(chunk + 4, count);
        memcpy(chunk + 12, f->clm + offset, count);
        result = variable(w, "clmload", 1, chunk, 12 + count, NULL, 0, NULL);
        if (result != HW_OK) return fail(w, result);
        offset += count;
    }
    w->boot_stage = "country configuration";
    unsigned char country[12] = {0}, applied_country[12] = {0}, status[4] = {0};
    country[0] = country[8] = (unsigned char)f->country[0];
    country[1] = country[9] = (unsigned char)f->country[1]; put32(country + 4, (uint32_t)f->country_revision);
    unsigned int actual = 0;
    const char* rejected_setting = NULL;
#define BOOT_CONTROL(stage, operation, required) do { \
        w->boot_stage = (stage); \
        result = (operation); \
        if (trace) trace(w->boot_stage, result, w->control_status); \
        if (result != HW_OK) { \
            if ((required) || !trace || !w->control_status) return fail(w, HW_IO); \
            if (!rejected_setting) rejected_setting = w->boot_stage; \
        } \
    } while (0)
    BOOT_CONTROL("CLM status", variable(w, "clmload_status", 0, NULL, 0, status, 4, &actual), 1);
    if (actual != 4 || le32(status)) return fail(w, HW_IO);
    BOOT_CONTROL("write country", variable(w, "country", 1, country, sizeof(country), NULL, 0, NULL), 1);
    BOOT_CONTROL("read country", variable(w, "country", 0, NULL, 0, applied_country, sizeof(applied_country), &actual), 1);
    if (actual != 12 || applied_country[0] != country[0] || applied_country[1] != country[1] ||
        applied_country[8] != country[8] || applied_country[9] != country[9] ||
        (f->country_revision >= 0 && le32(applied_country + 4) != (uint32_t)f->country_revision))
        return fail(w, HW_IO);
    BOOT_CONTROL("disable TX aggregation (bus:txglom)", var_int(w, "bus:txglom", 0), 0);
    BOOT_CONTROL("set power management (86)", command_int(w, 86, 0), 0);
    BOOT_CONTROL("set infrastructure mode (20)", command_int(w, 20, 1), 0);
    BOOT_CONTROL("disable roaming (roam_off)", var_int(w, "roam_off", 1), 0);
    /* Keep the radio powered while unassociated and select active probing. */
    BOOT_CONTROL("keep unassociated radio awake (mpc)", var_int(w, "mpc", 0), 0);
    BOOT_CONTROL("set active probing (49)", command_int(w, 49, 0), 0);
    unsigned char mask[16] = {0};
    const unsigned int events[] = {0, 5, 6, 11, 12, 16, 46, 69};
    for (unsigned int i = 0; i < sizeof(events) / sizeof(events[0]); i++) mask[events[i] / 8] |= (uint8_t)(1U << (events[i] % 8));
    BOOT_CONTROL("set event mask (event_msgs)", variable(w, "event_msgs", 1, mask, sizeof(mask), NULL, 0, NULL), 0);
    BOOT_CONTROL("read MAC address (cur_etheraddr)", variable(w, "cur_etheraddr", 0, NULL, 0, w->mac, 6, &actual), 1);
    if (actual != 6 || (w->mac[0] & 1) ||
        !(w->mac[0] | w->mac[1] | w->mac[2] | w->mac[3] | w->mac[4] | w->mac[5]))
        return fail(w, HW_IO);
    if (rejected_setting) {
        trace("bring radio up skipped after settings rejection", HW_OFF, 0);
        w->boot_stage = rejected_setting;
        return fail(w, HW_IO);
    }
    BOOT_CONTROL("bring radio up (2)", command_int(w, 2, 1), 1);
#undef BOOT_CONTROL
    erase(w->nvram, sizeof(w->nvram)); w->state = 1; w->error = 0;
    return HW_OK;
}

static int enter(Wifi* w) {
    if (!w || !w->bus.owned || !w->bus.owned(w->bus.context)) return HW_INVALID;
    if (w->state != 1) return HW_OFF;
    if (w->busy) return HW_BUSY;
    w->busy = 1;
    return HW_OK;
}

static int disassociate(Wifi* w) {
    unsigned char scb[12] = {0};
    return control(w, 52, 1, scb, sizeof(scb), NULL, 0, NULL);
}

/* Called with the driver entered. A completed join is verified against the
 * firmware's current SSID before frame transport is authorized. */
static void join_finish(Wifi* w, int result) {
    w->joining = 0;
    w->join_result = result;
    erase(w->join_ssid, sizeof(w->join_ssid));
    if (result == HW_OK) return;
    w->link = w->keyed = w->connected = 0; w->rx_count = w->rx_head = 0;
    if (w->state == 1) (void)disassociate(w);
}

static void join_progress(Wifi* w) {
    if (!w->joining || w->state != 1) return;
    if (w->join_error) { join_finish(w, HW_IO); return; }
    if (w->associated && w->link && (w->open || w->keyed)) {
        unsigned char current[36] = {0}; unsigned int actual = 0;
        int result = control(w, 25, 0, NULL, 0, current, sizeof(current), &actual);
        if (result == HW_OK && w->associated && w->link && (w->open || w->keyed) && !w->join_error &&
            actual == sizeof(current) && le32(current) == w->join_ssid_bytes &&
            !memcmp(current + 4, w->join_ssid, w->join_ssid_bytes)) {
            w->connected = 1;
            join_finish(w, HW_OK);
        } else join_finish(w, result == HW_OK || result == HW_IO ? HW_PROTOCOL : result);
        return;
    }
    if (w->bus.time_us(w->bus.context) - w->join_start >= 30000000U) join_finish(w, HW_TIMEOUT);
}

static void scan_progress(Wifi* w) {
    if (w->scanning && w->bus.time_us(w->bus.context) - w->scan_start >= 15000000U) {
        w->scanning = 0; w->scanned = 1;
    }
}

int wifi_poll(Wifi* w) {
    int result = enter(w);
    if (result != HW_OK) return result;
    result = pump(w);
    if (result >= 0) {
        join_progress(w);
        scan_progress(w);
        if (w->state != 1) result = w->error;
    }
    w->busy = 0;
    return result < 0 ? fail(w, result) : result;
}

int wifi_join_start(Wifi* w, const unsigned char* ssid, unsigned int ssid_bytes,
                     const unsigned char* passphrase, unsigned int passphrase_bytes, unsigned int flags) {
    if (!ssid || !ssid_bytes || ssid_bytes > 32 || (passphrase_bytes && !passphrase) ||
        (passphrase_bytes && (passphrase_bytes < 8 || passphrase_bytes > 63)) || (flags & ~WIFI_JOIN_HIDDEN))
        return HW_INVALID;
    for (unsigned int i = 0; i < passphrase_bytes; i++) if (passphrase[i] < 32 || passphrase[i] > 126) return HW_INVALID;
    int result = enter(w);
    if (result != HW_OK) return result;
    w->joining = 0; w->join_missing = 0;
    w->link = w->keyed = w->connected = 0; w->secure = 1; w->join_error = 0; w->rx_count = w->rx_head = 0;
    w->open = !passphrase_bytes;
    unsigned char pmk[68] = {0}, target[36] = {0};
    static const unsigned char rsn[] = {0x30, 0x14, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4,
                                        1, 0, 0, 0x0f, 0xac, 2, 0, 0};
    put32(target, ssid_bytes); memcpy(target + 4, ssid, ssid_bytes);
    int need_disassoc = (w->associated || w->link || w->connected) ? 1 : 0;
    if (w->open) {
        if ((need_disassoc ? disassociate(w) : HW_OK) != HW_OK || command_int(w, 22, 0) != HW_OK ||
            command_int(w, 134, 0) != HW_OK || var_int(w, "wpa_auth", 0) != HW_OK ||
            var_int(w, "sup_wpa", 0) != HW_OK) result = HW_IO;
    } else {
        put16(pmk, passphrase_bytes); put16(pmk + 2, 1); memcpy(pmk + 4, passphrase, passphrase_bytes);
        if ((need_disassoc ? disassociate(w) : HW_OK) != HW_OK ||
            command_int(w, 22, 0) != HW_OK || command_int(w, 134, 4) != HW_OK || var_int(w, "wpa_auth", 128) != HW_OK ||
            variable(w, "wpaie", 1, rsn, sizeof(rsn), NULL, 0, NULL) != HW_OK || var_int(w, "sup_wpa", 1) != HW_OK ||
            control(w, 268, 1, pmk, sizeof(pmk), NULL, 0, NULL) != HW_OK) result = HW_IO;
    }

    if (w->open) {
        /* Explicitly requested: open system authentication without encryption. */
        if ((w->associated ? disassociate(w) : HW_OK) != HW_OK || command_int(w, 22, 0) != HW_OK ||
            command_int(w, 134, 0) != HW_OK || var_int(w, "wpa_auth", 0) != HW_OK ||
            var_int(w, "sup_wpa", 0) != HW_OK) result = HW_IO;
    } else {
        put16(pmk, passphrase_bytes); put16(pmk + 2, 1); memcpy(pmk + 4, passphrase, passphrase_bytes);
        if ((w->associated ? disassociate(w) : HW_OK) != HW_OK ||
            command_int(w, 22, 0) != HW_OK || command_int(w, 134, 4) != HW_OK || var_int(w, "wpa_auth", 128) != HW_OK ||
            variable(w, "wpaie", 1, rsn, sizeof(rsn), NULL, 0, NULL) != HW_OK || var_int(w, "sup_wpa", 1) != HW_OK ||
            control(w, 268, 1, pmk, sizeof(pmk), NULL, 0, NULL) != HW_OK) result = HW_IO;
    }
    if (result == HW_OK) {
        /* Configuration commands may have drained events from a previous
         * association. Only events after this request can satisfy this join. */
        for (unsigned int drained = 0; drained < 64; drained++) {
            int old = pump(w);
            if (old < 0) { result = fail(w, old); goto join_done; }
            if (!old) break;
            if (drained == 63) { result = HW_BUSY; goto join_done; }
        }
        w->link = w->keyed = w->associated = w->join_error = w->join_missing = 0;
        int requested = HW_IO;
        if (flags & WIFI_JOIN_HIDDEN) {
            /* brcmf_ext_join_params_le up to an empty chanspec list: the SSID
             * with an active join scan, so the firmware sends directed probe
             * requests. Firmware without the "join" iovar falls back to
             * SET_SSID, as brcmf_cfg80211_connect does. */
            unsigned char join[68] = {0};
            memcpy(join, target, sizeof(target));
            for (unsigned int offset = 40; offset < 56; offset += 4) put32(join + offset, 0xffffffffU);
            memset(join + 56, 0xff, 6);
            requested = variable(w, "join", 1, join, sizeof(join), NULL, 0, NULL);
        }
        if (requested == HW_IO && w->state == 1) requested = control(w, 26, 1, target, sizeof(target), NULL, 0, NULL);
        if (requested != HW_OK) {
            result = HW_IO;
            goto join_done;
        }
        memcpy(w->join_ssid, ssid, ssid_bytes);
        w->join_ssid_bytes = ssid_bytes;
        w->join_start = w->bus.time_us(w->bus.context);
        w->joining = 1;
        w->join_result = HW_BUSY;
    }
join_done:
    erase(pmk, sizeof(pmk)); erase(w->tx, sizeof(w->tx)); erase(w->control, sizeof(w->control));
    if (result != HW_OK) {
        w->link = w->keyed = w->connected = 0;
        w->join_result = result;
        if (w->state == 1) (void)disassociate(w);
    }
    w->busy = 0;
    return result;
}

int wifi_join_status(const Wifi* w) {
    if (!w) return HW_INVALID;
    if (w->state != 1) return w->state == -1 ? w->error : HW_OFF;
    if (w->joining) return HW_BUSY;
    if (w->connected && w->link && (w->open || w->keyed)) return HW_OK;
    return w->join_result < 0 ? w->join_result : HW_OFF;
}

int wifi_join(Wifi* w, const unsigned char* ssid, unsigned int ssid_bytes,
               const unsigned char* passphrase, unsigned int passphrase_bytes) {
    if (!passphrase || passphrase_bytes < 8) return HW_INVALID;
    int result = wifi_join_start(w, ssid, ssid_bytes, passphrase, passphrase_bytes, 0);
    for (unsigned int i = 0; result == HW_OK && i < 1000000U; i++) {
        int polled = wifi_poll(w);
        if (polled < 0) return polled;
        int status = wifi_join_status(w);
        if (status != HW_BUSY) return status;
    }
    if (result != HW_OK) return result;
    if (enter(w) == HW_OK) { join_finish(w, HW_TIMEOUT); w->busy = 0; }
    return HW_TIMEOUT;
}

int wifi_disconnect(Wifi* w) {
    int result = enter(w);
    if (result != HW_OK) return result;
    result = disassociate(w);
    w->joining = 0; w->join_result = HW_OFF; w->join_missing = 0;
    erase(w->join_ssid, sizeof(w->join_ssid));
    w->link = w->keyed = w->connected = 0; w->rx_count = w->rx_head = 0; w->busy = 0;
    return result;
}

int wifi_scan_start(Wifi* w) {
    int result = enter(w);
    if (result != HW_OK) return result;
    /* brcmf_escan_params_le, version 1: all channels, any BSS type, defaults. */
    unsigned char params[72] = {0};
    put32(params, 1); put16(params + 4, 1); put16(params + 6, 0x1234);
    memset(params + 44, 0xff, 6);
    params[50] = 2;
    for (unsigned int offset = 52; offset < 68; offset += 4) put32(params + offset, 0xffffffffU);
    w->bss_count = 0; w->scanned = 0; w->scanning = 1;
    w->scan_start = w->bus.time_us(w->bus.context);
    result = variable(w, "escan", 1, params, sizeof(params), NULL, 0, NULL);
    if (result != HW_OK) {
        w->scanning = 0;
        if (w->state == 1 && result > 0) result = HW_IO;
    }
    w->busy = 0;
    return result;
}

int wifi_scan_status(const Wifi* w) {
    if (!w) return HW_INVALID;
    if (w->state != 1) return HW_OFF;
    return w->scanning ? HW_BUSY : w->scanned ? HW_OK : HW_OFF;
}

int wifi_rssi(Wifi* w, int32_t* dbm) {
    if (!dbm) return HW_INVALID;
    int result = enter(w);
    if (result != HW_OK) return result;
    if (!w->connected) { w->busy = 0; return HW_OFF; }
    unsigned char value[12] = {0}; unsigned int actual = 0;
    result = control(w, 127, 0, NULL, 0, value, sizeof(value), &actual);
    if (result == HW_OK && actual >= 4) *dbm = (int32_t)le32(value);
    else if (result == HW_OK) result = HW_PROTOCOL;
    w->busy = 0;
    return result;
}

int wifi_send(Wifi* w, const void* ethernet, unsigned int bytes) {
    if (!ethernet || bytes < 14 || bytes > WIFI_FRAME_MAX) return HW_INVALID;
    int result = enter(w);
    if (result != HW_OK) return result;
    if (!w->connected || !w->link || !(w->keyed || w->open)) { w->busy = 0; return HW_OFF; }
    result = credit(w, 2);
    if (result == HW_OK && (!w->connected || !w->link || !(w->keyed || w->open))) result = HW_OFF;
    if (result == HW_OK) {
        memset(w->tx, 0, (bytes + 16 + 3U) & ~3U); header(w, bytes + 16, 2); w->tx[12] = 0x20;
        memcpy(w->tx + 16, ethernet, bytes);
        result = fifo(w, 1, w->tx, (bytes + 16 + 3U) & ~3U);
        if (result == HW_OK) w->tx_sequence++;
    }
    w->busy = 0;
    erase(w->tx, sizeof(w->tx));
    return result < 0 && result != HW_OFF ? fail(w, result) : result;
}

int wifi_receive(Wifi* w, void* ethernet, unsigned int capacity) {
    if (!ethernet) return HW_INVALID;
    int result = enter(w);
    if (result != HW_OK) return result;
    result = 0;
    if (w->rx_count) {
        unsigned int bytes = w->frame_bytes[w->rx_head];
        if (capacity < bytes) result = HW_NO_SPACE;
        else {
            memcpy(ethernet, w->frames[w->rx_head], bytes); result = (int)bytes;
            w->rx_head = (w->rx_head + 1) % 4; w->rx_count--;
        }
    }
    w->busy = 0;
    return result;
}
