#include <vantage/drivers/bluetooth.h>

/* Bluetooth Core Specification, Vol. 4 Parts A and E. */
enum {
    H4_COMMAND = 0x01, H4_ACL = 0x02, H4_SCO = 0x03, H4_EVENT = 0x04, H4_ISO = 0x05,
    HCI_COMMAND_COMPLETE = 0x0e, HCI_COMMAND_STATUS = 0x0f, HCI_HARDWARE_ERROR = 0x10,
    HCI_LE_META = 0x3e, HCI_LE_ADVERTISING_REPORT = 0x02,
    AD_SHORT_NAME = 0x08, AD_COMPLETE_NAME = 0x09
};

enum {
    HCI_IDLE, HCI_RESET, HCI_RESET_SETTLING,
    HCI_READ_VERSION, HCI_DOWNLOAD_MINIDRIVER, HCI_MINIDRIVER_SETTLING,
    HCI_PATCH_RECORD, HCI_PATCH_SETTLING, HCI_EVENT_MASK, HCI_LE_EVENT_MASK,
    HCI_SCAN_PARAMETERS, HCI_SCAN_ENABLE, HCI_SCAN_ACTIVE, HCI_SCAN_DISABLE
};

/* Infineon AIROC HCI UART Control Protocol, sections 3.3 and 3.4. */
enum {
    BCM_WRITE_RAM = 0xfc4c, BCM_LAUNCH_RAM = 0xfc4e,
    BCM_COMPANY = 0x000f, BCM4345C0_SUBVERSION = 0x6119
};

#define COMMAND_US 1000000U
#define MINIDRIVER_SETTLE_US 50000U
#define PATCH_SETTLE_US 250000U
/* FIFO servicing is paced by the platform poll, which can run once per frame. */
#define PATCH_TIMEOUT_US 180000000U
#define PATCH_MAX_BYTES (64U * 1024U)
#define HCD_HEADER 3U
#define FRAME_US 250000U
#define SCAN_US 5000000U
#define POLL_BYTES 256U
#define REPORT_OVERHEAD 10U
#define LEGACY_AD_MAX 31U

static const struct {
    uint16_t opcode;
    uint8_t size, next;
    uint8_t data[8];
} commands[] = {
    [HCI_RESET] = {0x0c03, 0, HCI_RESET_SETTLING, {0}},
    [HCI_READ_VERSION] = {0x1001, 0, HCI_DOWNLOAD_MINIDRIVER, {0}},
    [HCI_DOWNLOAD_MINIDRIVER] = {0xfc2e, 0, HCI_MINIDRIVER_SETTLING, {0}},
    /* Event mask bits 15 (Hardware Error) and 61 (LE Meta Event). */
    [HCI_EVENT_MASK] = {0x0c01, 8, HCI_LE_EVENT_MASK, {0, 0x80, 0, 0, 0, 0, 0, 0x20}},
    [HCI_LE_EVENT_MASK] = {0x2001, 8, HCI_IDLE, {0x02}},
    /* Active, 60 ms interval, 30 ms window, public own address, accept all. */
    [HCI_SCAN_PARAMETERS] = {0x200b, 7, HCI_SCAN_ENABLE, {1, 0x60, 0, 0x30, 0, 0, 0}},
    [HCI_SCAN_ENABLE] = {0x200c, 2, HCI_SCAN_ACTIVE, {1, 0}},
    [HCI_SCAN_DISABLE] = {0x200c, 2, HCI_IDLE, {0, 0}}
};

static uint16_t read_le16(const uint8_t* data) {
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static int patch_valid(const BtPatch* patch) {
    if (!patch->data || patch->size > PATCH_MAX_BYTES) return 0;
    size_t offset = 0;
    while (patch->size - offset >= HCD_HEADER) {
        const uint8_t* record = patch->data + offset;
        uint16_t opcode = read_le16(record);
        size_t size = record[2];
        offset += HCD_HEADER;
        if (size > patch->size - offset) return 0;
        if (opcode == BCM_LAUNCH_RAM)
            return offset > HCD_HEADER && size == 4 && size == patch->size - offset;
        if (opcode != BCM_WRITE_RAM || size <= 4) return 0;
        offset += size;
    }
    return 0;
}

static void set_step(BtHci* hci, uint8_t step, uint32_t now) {
    hci->step = step;
    hci->step_started = now;
    hci->command_sent = 0;
}

static void fail(BtHci* hci, int error) {
    hci->status.state = BT_FAILED;
    hci->status.count = 0;
    hci->status.error = error;
}

static void update_name(BtHci* hci, uint32_t index, const uint8_t* data, size_t size) {
    const uint8_t* name = NULL;
    uint8_t name_type = hci->name_types[index];
    size_t name_size = 0;
    while (size) {
        uint8_t length = *data++;
        size--;
        if (!length) break;
        if (length > size) return;
        if ((data[0] == AD_SHORT_NAME || data[0] == AD_COMPLETE_NAME) &&
            data[0] >= name_type && length > 1 && data[1]) {
            name_type = data[0];
            name = data + 1;
            name_size = length - 1U;
        }
        data += length;
        size -= length;
    }
    if (name) {
        BtDevice* device = &hci->devices[index];
        memset(device->name, 0, sizeof(device->name));
        memcpy(device->name, name, name_size);
        hci->name_types[index] = name_type;
    }
}

static void record_report(BtHci* hci, const uint8_t* report) {
    /* Each legacy record includes its own event type, address and data length. */
    if (report[0] > 4 || report[1] > 3) return;
    uint32_t index = 0;
    for (; index < hci->status.count; index++) {
        const BtDevice* device = &hci->devices[index];
        if (device->address_type == report[1] &&
            memcmp(device->address, report + 2, sizeof(device->address)) == 0) break;
    }
    if (index == BT_DEVICE_LIMIT) return;
    BtDevice* device = &hci->devices[index];
    if (index == hci->status.count) {
        memset(device, 0, sizeof(*device));
        memcpy(device->address, report + 2, sizeof(device->address));
        device->address_type = report[1];
        hci->name_types[index] = 0;
        hci->status.count++;
    }
    uint8_t data_size = report[8];
    uint8_t rssi = report[9 + data_size];
    device->rssi = rssi < 128 ? rssi : (int32_t)rssi - 256;
    update_name(hci, index, report + 9, data_size);
}

static void advertising_reports(BtHci* hci, const uint8_t* data, size_t size) {
    if (size < 2 || !data[1]) return;
    uint8_t count = data[1];
    data += 2;
    size -= 2;

    /* Validate the entire event layout before publishing any results. */
    const uint8_t* report = data;
    for (unsigned int i = 0; i < count; i++) {
        if (size < REPORT_OVERHEAD || report[8] > LEGACY_AD_MAX ||
            report[8] > size - REPORT_OVERHEAD) return;
        size_t record_size = REPORT_OVERHEAD + report[8];
        report += record_size;
        size -= record_size;
    }
    if (size) return;
    for (unsigned int i = 0; i < count; i++) {
        record_report(hci, data);
        data += REPORT_OVERHEAD + data[8];
    }
}

static void command_event(BtHci* hci, uint8_t event, const uint8_t* data,
                          size_t size, uint32_t now) {
    int complete = event == HCI_COMMAND_COMPLETE;
    if ((complete && size < 3) || (!complete && size != 4)) {
        fail(hci, BT_PROTOCOL);
        return;
    }
    hci->command_credits = data[complete ? 0 : 1];
    uint16_t opcode = read_le16(data + (complete ? 1 : 2));
    if (!hci->command_sent || opcode != bt_hci_pending_opcode(hci)) return;
    if (!complete) {
        if (data[0]) fail(hci, BT_PROTOCOL);
        return;
    }
    size_t expected = hci->step == HCI_READ_VERSION ? 12U : 4U;
    if (size != expected || data[3]) {
        fail(hci, BT_PROTOCOL);
        return;
    }

    uint8_t next = commands[hci->step].next;
    if (hci->step == HCI_READ_VERSION) {
        /* Read Local Version Information, Bluetooth Core Vol. 2, Part E. */
        hci->manufacturer = read_le16(data + 8);
        hci->rom_subversion = read_le16(data + 10);
        if (hci->manufacturer != BCM_COMPANY || hci->rom_subversion != BCM4345C0_SUBVERSION) {
            fail(hci, BT_UNAVAILABLE);
            return;
        }
    } else if (hci->step == HCI_PATCH_RECORD) {
        hci->patch_offset += HCD_HEADER + hci->patch.data[hci->patch_offset + 2];
        next = opcode == BCM_LAUNCH_RAM ? HCI_PATCH_SETTLING : HCI_PATCH_RECORD;
    }
    if (next == HCI_IDLE) hci->status.state = BT_READY;
    set_step(hci, next, now);
}

static void finish_frame(BtHci* hci, uint32_t now) {
    uint8_t type = hci->rx_type;
    hci->rx_type = 0;
    if (type != H4_EVENT) return;

    uint8_t event = hci->rx_header[0];
    uint8_t size = hci->rx_header[1];
    const uint8_t* data = hci->rx_event;
    switch (event) {
    case HCI_COMMAND_COMPLETE:
    case HCI_COMMAND_STATUS:
        command_event(hci, event, data, size, now);
        break;
    case HCI_HARDWARE_ERROR:
        fail(hci, size == 1 ? BT_IO : BT_PROTOCOL);
        break;
    case HCI_LE_META:
        if (hci->step == HCI_SCAN_ACTIVE && size && data[0] == HCI_LE_ADVERTISING_REPORT)
            advertising_reports(hci, data, size);
        break;
    }
}

static void receive_byte(BtHci* hci, uint8_t byte, uint32_t now) {
    static const uint8_t header_sizes[] = {0, 0, 4, 3, 2, 4};
    if (!hci->rx_type) {
        if (byte < H4_ACL || byte > H4_ISO) {
            fail(hci, BT_PROTOCOL);
            return;
        }
        hci->rx_type = byte;
        hci->rx_header_used = 0;
        hci->frame_started = now;
        return;
    }

    if (hci->rx_header_used < header_sizes[hci->rx_type]) {
        hci->rx_header[hci->rx_header_used++] = byte;
        if (hci->rx_header_used < header_sizes[hci->rx_type]) return;
        /* Skip data packets at their H4 boundaries. Only events are buffered. */
        switch (hci->rx_type) {
        case H4_EVENT: hci->rx_remaining = hci->rx_header[1]; break;
        case H4_SCO: hci->rx_remaining = hci->rx_header[2]; break;
        case H4_ACL: hci->rx_remaining = read_le16(hci->rx_header + 2); break;
        case H4_ISO: hci->rx_remaining = read_le16(hci->rx_header + 2) & 0x3fffU; break;
        }
    } else {
        if (hci->rx_type == H4_EVENT)
            hci->rx_event[hci->rx_header[1] - hci->rx_remaining] = byte;
        hci->rx_remaining--;
    }
    if (!hci->rx_remaining) finish_frame(hci, now);
}

static void check_deadlines(BtHci* hci, uint32_t now) {
    uint32_t elapsed = now - hci->step_started;
    if (hci->rx_type && now - hci->frame_started >= FRAME_US) {
        fail(hci, BT_TIMEOUT);
    } else if (hci->step >= HCI_READ_VERSION && hci->step <= HCI_PATCH_SETTLING &&
               now - hci->patch_started >= PATCH_TIMEOUT_US) {
        fail(hci, BT_TIMEOUT);
    } else if (hci->step == HCI_RESET_SETTLING) {
        if (elapsed < hci->io.reset_settle_us) return;
        if (hci->patch_offset < hci->patch.size) {
            hci->patch_started = now;
            set_step(hci, HCI_READ_VERSION, now);
        } else {
            set_step(hci, HCI_EVENT_MASK, now);
        }
    } else if (hci->step == HCI_MINIDRIVER_SETTLING) {
        if (elapsed >= MINIDRIVER_SETTLE_US) set_step(hci, HCI_PATCH_RECORD, now);
    } else if (hci->step == HCI_PATCH_SETTLING) {
        if (elapsed >= PATCH_SETTLE_US) set_step(hci, HCI_RESET, now);
    } else if (hci->step == HCI_SCAN_ACTIVE) {
        if (elapsed >= SCAN_US) set_step(hci, HCI_SCAN_DISABLE, now);
    } else if (hci->step != HCI_IDLE && elapsed >= COMMAND_US) {
        fail(hci, BT_TIMEOUT);
    }
}

static int transport_error(int result) {
    if (result == BT_UNAVAILABLE || result == BT_TIMEOUT || result == BT_PROTOCOL) return result;
    return BT_IO;
}

static void send_command(BtHci* hci) {
    uint16_t opcode = bt_hci_pending_opcode(hci);
    uint8_t size = commands[hci->step].size;
    const uint8_t* data = commands[hci->step].data;
    if (hci->step == HCI_PATCH_RECORD) {
        const uint8_t* record = hci->patch.data + hci->patch_offset;
        size = record[2];
        data = record + HCD_HEADER;
    }
    uint8_t packet[4 + 255];
    packet[0] = H4_COMMAND;
    packet[1] = (uint8_t)opcode;
    packet[2] = (uint8_t)(opcode >> 8);
    packet[3] = size;
    memcpy(packet + 4, data, size);
    int result = hci->io.send(hci->io.context, packet, 4U + size);
    if (result == BT_OK) {
        hci->command_credits--;
        hci->command_sent = 1;
    } else if (result != BT_BUSY) {
        fail(hci, transport_error(result));
    }
}

int bt_hci_init(BtHci* hci, const BtH4Io* io, const BtPatch* patch) {
    if (!hci) return BT_UNAVAILABLE;
    BtH4Io transport = {0};
    BtPatch firmware = {0};
    if (io) transport = *io;
    if (patch) firmware = *patch;
    memset(hci, 0, sizeof(*hci));
    if (!transport.send || !transport.receive || !transport.time_us) {
        hci->status.error = BT_UNAVAILABLE;
        return BT_UNAVAILABLE;
    }
    if (patch && !patch_valid(&firmware)) {
        fail(hci, BT_PROTOCOL);
        return BT_PROTOCOL;
    }
    hci->io = transport;
    hci->patch = firmware;
    hci->status.state = BT_BOOTING;
    hci->command_credits = 1;
    set_step(hci, HCI_RESET, hci->io.time_us(hci->io.context));
    return BT_OK;
}

void bt_hci_poll(BtHci* hci) {
    if (!hci || hci->status.state == BT_DISABLED || hci->status.state == BT_FAILED) return;

    uint32_t now = hci->io.time_us(hci->io.context);
    /* Boot self-tests can delay servicing Reset after it was queued. */
    if (!hci->polled) {
        hci->step_started = now;
        hci->polled = 1;
    }
    check_deadlines(hci, now);
    for (unsigned int i = 0; i < POLL_BYTES && hci->status.state != BT_FAILED; i++) {
        uint8_t byte;
        int result = hci->io.receive(hci->io.context, &byte);
        if (!result) break;
        if (result != 1) {
            fail(hci, transport_error(result));
            return;
        }
        receive_byte(hci, byte, now);
    }
    if (hci->status.state == BT_FAILED) return;

    now = hci->io.time_us(hci->io.context);
    check_deadlines(hci, now);
    if (hci->status.state != BT_FAILED && bt_hci_pending_opcode(hci) &&
        !hci->command_sent && hci->command_credits)
        send_command(hci);
}

uint16_t bt_hci_pending_opcode(const BtHci* hci) {
    if (hci->step == HCI_PATCH_RECORD)
        return read_le16(hci->patch.data + hci->patch_offset);
    return commands[hci->step].opcode;
}

int bt_hci_scan_start(BtHci* hci) {
    if (!hci || hci->status.state == BT_DISABLED) return BT_UNAVAILABLE;
    if (hci->status.state == BT_FAILED) return hci->status.error;
    if (hci->status.state != BT_READY) return BT_BUSY;

    hci->status.count = 0;
    hci->status.state = BT_SCANNING;
    set_step(hci, HCI_SCAN_PARAMETERS, hci->io.time_us(hci->io.context));
    return BT_OK;
}

void bt_hci_status(const BtHci* hci, BtStatus* status) {
    if (status) *status = hci ? hci->status : (BtStatus){0};
}

int bt_hci_device(const BtHci* hci, uint32_t index, BtDevice* device) {
    if (!device || index >= BT_DEVICE_LIMIT) return BT_PROTOCOL;
    if (!hci || hci->status.state == BT_DISABLED) return BT_UNAVAILABLE;
    if (hci->status.state == BT_FAILED) return hci->status.error;
    if (index >= hci->status.count) return 0;
    *device = hci->devices[index];
    return BT_OK;
}
