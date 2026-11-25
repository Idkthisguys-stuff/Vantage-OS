#include "home.h"
#include "keyboard.h"
#include "terminal.h"
#include <vantage/user.h>

#define BG RGB(13, 24, 29)
#define TEXT RGB(232, 245, 242)
#define MUTED RGB(156, 188, 185)
#define TILE RGB(30, 61, 67)
#define ACTIVE RGB(38, 103, 100)

enum { HOME, TERMINAL, FILES, NETWORK, BLUETOOTH, USB_FILES, USB_PREVIEW, WIFI_PASSWORD };
enum { NONE, OPEN_TERMINAL, OPEN_FILES, OPEN_NETWORK, OPEN_BLUETOOTH,
       BACK, PREVIOUS, MORE, REFRESH, SCAN, OPEN_USB, USB_BACK, USB_PREVIOUS,
       USB_MORE, PREVIEW_MORE, WIFI_SCAN, WIFI_PREVIOUS, WIFI_MORE, WIFI_CANCEL,
       WIFI_JOIN, USB_ITEM = 64, WIFI_ITEM = 128 };
enum { FILE_ROWS = 12, SCAN_BUDGET = FILE_ROWS + 1 };
enum { USB_ROWS = 12, USB_FIRST_ROW = 12, PREVIEW_FIRST_ROW = 12, PREVIEW_ROWS = 30,
       PREVIEW_COLUMNS = 54, WIFI_ROWS = 10, WIFI_FIRST_ROW = 22 };
enum { BT_POLL_US = 250000, BT_FIRST_DEVICE_ROW = 15 };
enum { HOME_LEFT = 24, HOME_TOP = 40, HOME_TILE_WIDTH = 102, HOME_TILE_HEIGHT = 52,
       HOME_TILE_PITCH = HOME_TILE_WIDTH + 8 };

typedef struct {
    uint32_t action, x, y, width, height;
    const char* label;
} Control;

static const Control home_controls[] = {
    {OPEN_TERMINAL, HOME_LEFT, HOME_TOP, HOME_TILE_WIDTH, HOME_TILE_HEIGHT, "Console"},
    {OPEN_FILES, HOME_LEFT + HOME_TILE_PITCH, HOME_TOP, HOME_TILE_WIDTH, HOME_TILE_HEIGHT, "Files"},
    {OPEN_NETWORK, HOME_LEFT + 2 * HOME_TILE_PITCH, HOME_TOP, HOME_TILE_WIDTH, HOME_TILE_HEIGHT, "Network"},
    {OPEN_BLUETOOTH, HOME_LEFT + 3 * HOME_TILE_PITCH, HOME_TOP, HOME_TILE_WIDTH, HOME_TILE_HEIGHT, "Bluetooth"},
    {NONE, 0, 0, 0, 0, ""}
};
static const Control file_controls[] = {
    {BACK, 24, 32, 90, 40, "Home"},
    {OPEN_USB, 356, 32, 100, 40, "USB"},
    {PREVIOUS, 24, 704, 132, 48, "Prev"},
    {MORE, 324, 704, 132, 48, "Next"},
    {NONE, 0, 0, 0, 0, ""}
};
static const Control usb_controls[] = {
    {BACK, 24, 32, 90, 40, "Home"},
    {OPEN_FILES, 344, 32, 112, 40, "Device"},
    {USB_PREVIOUS, 24, 704, 132, 48, "Prev"},
    {USB_MORE, 324, 704, 132, 48, "Next"},
    {NONE, 0, 0, 0, 0, ""}
};
static const Control preview_controls[] = {
    {USB_BACK, 24, 32, 90, 40, "USB"},
    {PREVIEW_MORE, 324, 704, 132, 48, "Next"},
    {NONE, 0, 0, 0, 0, ""}
};
static const Control network_controls[] = {
    {BACK, 24, 32, 90, 40, "Home"},
    {REFRESH, 24, 288, 132, 48, "Refresh"},
    {WIFI_SCAN, 324, 288, 132, 48, "Scan"},
    {WIFI_PREVIOUS, 24, 704, 132, 48, "Prev"},
    {WIFI_MORE, 324, 704, 132, 48, "Next"},
    {NONE, 0, 0, 0, 0, ""}
};
static const Control password_controls[] = {
    {WIFI_CANCEL, 24, 32, 100, 40, "Back"},
    {WIFI_JOIN, 356, 32, 100, 40, "Join"},
    {NONE, 0, 0, 0, 0, ""}
};
static const Control bluetooth_controls[] = {
    {BACK, 24, 32, 90, 40, "Home"},
    {SCAN, 24, 176, 132, 48, "Scan"},
    {NONE, 0, 0, 0, 0, ""}
};

static uint32_t view, dirty, pressed, contact_id;
static uint32_t file_page, file_more;
static uint32_t usb_generation, usb_state, usb_count, usb_scanning, usb_page;
static UsbFileCall preview;
static uint32_t preview_pos, preview_next;
static int preview_error;
static NetStatus network_status;
static NetScan networks[NET_SCAN_LIMIT];
static uint32_t wifi_count, wifi_page, wifi_scanning, wifi_selected, password_length;
static int wifi_error, wifi_scan_done;
static char password[NET_PASSPHRASE_MAX + 1];
static uint64_t clock_minute;
static uint32_t clock_synced;
static BtStatus bluetooth_status;
static uint32_t bluetooth_polled, bluetooth_scan_failed;

static void text(uint32_t row, const char* content, uint32_t color) {
    TextRow output = { .row = row, .length = 60, .foreground = color,
                       .background = BG, .cursor = TEXT_NO_CURSOR };
    uint32_t col = 0;
    while (col < output.length) output.text[col++] = ' ';
    col = 3;
    while (*content && col < output.length) {
        unsigned char ch = (unsigned char)*content++;
        output.text[col++] = ch >= 32 && ch <= 126 ? (char)ch : '?';
    }
    sys_canvas_text_row(&output);
}

static const Control* controls(void) {
    if (view == FILES) return file_controls;
    if (view == USB_FILES) return usb_controls;
    if (view == USB_PREVIEW) return preview_controls;
    if (view == NETWORK) return network_controls;
    if (view == WIFI_PASSWORD) return password_controls;
    if (view == BLUETOOTH) return bluetooth_controls;
    return home_controls;
}

static uint32_t enabled(uint32_t action) {
    if (action == SCAN) return bluetooth_status.state == BT_READY;
    if (action == USB_PREVIOUS) return usb_page != 0;
    if (action == USB_MORE) return (usb_page + 1) * USB_ROWS < usb_count;
    if (action == PREVIEW_MORE)
        return !preview_error && (preview_next < preview.length || preview.offset + preview.length < preview.size);
    if (action == WIFI_SCAN) return network_status.available && !wifi_scanning;
    if (action == WIFI_PREVIOUS) return wifi_page != 0;
    if (action == WIFI_MORE) return (wifi_page + 1) * WIFI_ROWS < wifi_count;
    if (action == WIFI_JOIN) return password_length >= NET_PASSPHRASE_MIN;
    return (action != PREVIOUS || file_page) && (action != MORE || file_more);
}

static void poll_usb(void) {
    UsbFileCall call = { .operation = USB_FILES_STATUS };
    if (sys_usb_files(&call) < 0) return;
    if (view == USB_PREVIEW && (preview.generation != call.generation || call.state != USB_MEDIA_READY)) {
        if (preview_error != FILE_UNAVAILABLE) dirty = 1;
        preview_error = FILE_UNAVAILABLE;
    }
    if (usb_generation != call.generation || usb_state != call.state ||
        usb_count != call.count || usb_scanning != call.scanning) dirty = 1;
    if (usb_generation != call.generation || usb_state != call.state) {
        usb_page = 0;
        if (view == USB_FILES) { pressed = NONE; contact_id = TOUCH_CONTACTS_MAX; }
    }
    usb_generation = call.generation;
    usb_state = call.state;
    usb_count = call.count;
    usb_scanning = call.scanning;
}

static void draw_usb_files(void) {
    text(7, "USB files", TEXT);
    if (usb_state == USB_MEDIA_ABSENT) { text(9, "Plug in a FAT32 USB drive", MUTED); return; }
    if (usb_state == USB_MEDIA_UNSUPPORTED) { text(9, "Unsupported volume. Use FAT32.", MUTED); return; }
    if (usb_state == USB_MEDIA_ERROR) { text(9, "Could not read the USB drive", MUTED); return; }
    text(9, usb_scanning ? "Reading the directory..." :
         usb_count ? "Read-only" : "No files in the root directory", MUTED);
    for (uint32_t row = 0; row < USB_ROWS && usb_page * USB_ROWS + row < usb_count; row++) {
        UsbFileCall call = { .operation = USB_FILES_ENTRY, .generation = usb_generation,
                             .index = usb_page * USB_ROWS + row };
        if (sys_usb_files(&call) == 0) text(USB_FIRST_ROW + row * 2, call.name, TEXT);
    }
}

static void draw_preview(void) {
    text(7, preview.name[0] ? preview.name : "USB file", TEXT);
    if (preview_error) {
        text(9, preview_error == FILE_UNAVAILABLE ? "USB drive removed" :
             terminal_file_error_text(preview_error), MUTED);
        return;
    }
    if (!preview.size) { text(9, "Empty file", MUTED); return; }
    uint32_t pos = preview_pos;
    for (uint32_t row = 0; row < PREVIEW_ROWS && pos < preview.length; row++) {
        char line[PREVIEW_COLUMNS + 1];
        uint32_t col = 0;
        while (col < PREVIEW_COLUMNS && pos < preview.length) {
            uint8_t ch = preview.data[pos++];
            if (ch == '\n') break;
            if (ch != '\r') line[col++] = ch >= 32 && ch <= 126 ? (char)ch : '.';
        }
        line[col] = 0;
        text(PREVIEW_FIRST_ROW + row, line, TEXT);
    }
    preview_next = pos;
}

static void read_preview(uint32_t index, uint32_t offset, uint32_t generation) {
    preview = (UsbFileCall){ .operation = USB_FILES_READ, .generation = generation,
                             .index = index, .offset = offset };
    preview_error = sys_usb_files(&preview);
    preview_pos = preview_next = 0;
}

static void poll_clock(void) {
    NetTime time = {0};
    uint32_t synced = sys_net_time(&time) > 0 && time.synced;
    uint64_t minute = time.unix_seconds / 60U;
    if (synced != clock_synced || (synced && minute != clock_minute)) dirty = 1;
    clock_synced = synced;
    clock_minute = minute;
}

static void draw_clock(void) {
    if (!clock_synced) { text(7, "Clock waiting for network time", MUTED); return; }
    uint32_t hour = (uint32_t)(clock_minute / 60U % 24U), minute = (uint32_t)(clock_minute % 60U);
    char display[] = "00:00 UTC";
    display[0] = (char)('0' + hour / 10U);
    display[1] = (char)('0' + hour % 10U);
    display[3] = (char)('0' + minute / 10U);
    display[4] = (char)('0' + minute % 10U);
    text(7, display, TEXT);
}

static void draw_controls(void) {
    for (const Control* control = controls(); control->action; control++) {
        if (!enabled(control->action)) continue;
        CanvasButton button = { .x = control->x, .y = control->y,
                                .width = control->width, .height = control->height,
                                .radius = 10, .foreground = TEXT,
                                .background = control->action == pressed ? ACTIVE : TILE };
        uint32_t length = 0;
        while (control->label[length]) length++;
        if (length < sizeof(button.label)) memcpy(button.label, control->label, length);
        sys_canvas_button(&button);
        if (length < sizeof(button.label)) continue;

        /* Center labels wider than the ABI field using adjacent text segments. */
        if (length * TEXT_CELL_WIDTH > button.width) continue;
        button.x += (button.width - length * TEXT_CELL_WIDTH) / 2;
        button.y += (button.height - TEXT_CELL_HEIGHT) / 2;
        button.height = TEXT_CELL_HEIGHT;
        button.radius = 0;
        for (uint32_t offset = 0; offset < length;) {
            uint32_t count = length - offset;
            if (count >= sizeof(button.label)) count = sizeof(button.label) - 1;
            memcpy(button.label, control->label + offset, count);
            button.label[count] = '\0';
            button.width = count * TEXT_CELL_WIDTH;
            sys_canvas_button(&button);
            button.x += button.width;
            offset += count;
        }
    }
}

static void draw_files(void) {
    text(7, "Files", TEXT);
    text(9, "Device storage", MUTED);
    FileDirectoryLong request = { .operation = DIRECTORY_LIST, .path = "/ram" };
    int result = 0;
    uint32_t shown = 0;
    uint32_t first = file_page * FILE_ROWS;
    file_more = 0;

    for (uint32_t i = 0; i < first + SCAN_BUDGET; i++) {
        result = sys_file_directory_long(&request);
        if (result <= 0) break;
        if (i < first) continue;
        if (shown == FILE_ROWS) { file_more = 1; break; }
        text(12 + shown * 2, request.entry, TEXT);
        shown++;
    }
    if (result < 0) text(12, terminal_file_error_text(result), MUTED);
    else if (!shown) text(12, "No files in device storage", MUTED);
}

static void address_text(uint32_t address, char output[16]) {
    unsigned int used = 0;
    for (unsigned int part = 0; part < 4; part++) {
        unsigned int octet = (address >> (24 - part * 8)) & 255U;
        if (part) output[used++] = '.';
        if (octet >= 100) output[used++] = (char)('0' + octet / 100);
        if (octet >= 10) output[used++] = (char)('0' + octet / 10 % 10);
        output[used++] = (char)('0' + octet % 10);
    }
    output[used] = '\0';
}

static void poll_network(void) {
    NetStatus status = {0};
    if (sys_net_status(&status) < 0) status = (NetStatus){0};
    status.ssid[NET_SSID_MAX] = '\0';
    uint32_t connected = status.available && status.link == NET_LINK_UP;
    uint32_t was_connected = network_status.available && network_status.link == NET_LINK_UP;
    uint32_t bound = status.configuration == NET_ADDRESS_BOUND;

    if (status.available != network_status.available || connected != was_connected ||
        status.link != network_status.link || status.error != network_status.error ||
        (connected && memcmp(status.ssid, network_status.ssid, NET_SSID_MAX)) ||
        status.ethernet != network_status.ethernet || status.configuration != network_status.configuration ||
        (bound && status.address != network_status.address)) dirty = 1;
    network_status = status;
}

static void fetch_networks(void) {
    if (!wifi_scanning) return;
    NetScan request = { .operation = NET_SCAN_RESULT, .cursor = wifi_count };
    while (wifi_count < NET_SCAN_LIMIT) {
        int result = sys_net_scan(&request);
        if (result == NET_AGAIN) return;
        if (result <= 0) {
            wifi_error = result;
            break;
        }
        if (!request.ssid_length || request.ssid_length > NET_SSID_MAX) {
            wifi_error = NET_BAD;
            break;
        }
        request.ssid[request.ssid_length] = 0;
        networks[wifi_count++] = request;
    }
    wifi_scan_done = 1;
    wifi_scanning = 0;
    dirty = 1;
}

static int supported_network(uint32_t security) {
    return security == NET_SECURITY_OPEN || security == NET_SECURITY_WPA2 || security == NET_SECURITY_WPA2_WPA3;
}

static void clear_password(void) {
    volatile char* bytes = password;
    for (uint32_t i = 0; i < sizeof(password); i++) bytes[i] = 0;
    password_length = 0;
    if (view == WIFI_PASSWORD) keyboard_cancel_touches();
}

static void join_network(void) {
    wifi_error = sys_net_join(networks[wifi_selected].ssid, password, 0);
    clear_password();
    view = NETWORK;
    poll_network();
}

static void select_network(uint32_t index) {
    if (index >= wifi_count || wifi_scanning) return;
    wifi_selected = index;
    const NetScan* selected = &networks[index];
    wifi_error = 0;
    for (uint32_t i = 0; i < selected->ssid_length; i++)
        if (!selected->ssid[i]) { wifi_error = NET_UNSUPPORTED; return; }
    if (!supported_network(selected->security)) { wifi_error = NET_UNSUPPORTED; return; }
    clear_password();
    if (selected->security == NET_SECURITY_OPEN) { join_network(); return; }
    ConsoleInfo info = {0};
    sys_console_info(&info);
    keyboard_init(info.columns, info.rows);
    keyboard_enable_symbols();
    view = WIFI_PASSWORD;
}

static void draw_password(void) {
    text(7, "Join Wi-Fi", TEXT);
    text(9, networks[wifi_selected].ssid, TEXT);
    text(12, "Enter Wi-Fi password", MUTED);
    char hidden[NET_PASSPHRASE_MAX + 1];
    for (uint32_t i = 0; i < password_length; i++) hidden[i] = '*';
    hidden[password_length] = 0;
    if (password_length > 54) {
        text(15, hidden + 54, TEXT);
        hidden[54] = 0;
    }
    text(14, hidden, TEXT);
    keyboard_redraw();
    keyboard_draw();
}

static void draw_network(void) {
    text(7, "Network", TEXT);
    const NetStatus* status = &network_status;

    if (!status->available) text(9, "Wi-Fi unavailable", MUTED);
    else if (status->link == NET_LINK_UP) {
        text(9, "Wi-Fi connected", TEXT);
        text(10, status->ssid, MUTED);
    } else if (status->link == NET_LINK_JOINING) text(9, "Joining Wi-Fi...", TEXT);
    else if (status->link == NET_LINK_RETRYING) text(9, "Reconnecting to Wi-Fi...", TEXT);
    else text(9, "Wi-Fi disconnected", MUTED);

    text(12, status->ethernet == NET_ETHERNET_UP ? "Ethernet connected" :
             status->ethernet == NET_ETHERNET_NO_CABLE ? "Ethernet cable unplugged" :
             "Ethernet unavailable", MUTED);

    if (status->configuration == NET_ADDRESS_BOUND) {
        char address[16];
        address_text(status->address, address);
        text(14, "IP address", TEXT);
        text(15, address, MUTED);
    } else text(14, status->configuration == NET_ADDRESS_CONFIGURING ? "Requesting IP address..." :
                "No IP address", MUTED);
    int error = wifi_error ? wifi_error : status->error;
    if (error) text(16, error == NET_AUTH ? "Wi-Fi authentication failed" :
                       error == NET_UNSUPPORTED ? "This network requires unsupported security" :
                       "Wi-Fi operation failed. Try again.", MUTED);
    if (wifi_scanning) { text(WIFI_FIRST_ROW, "Scanning...", MUTED); return; }
    if (wifi_scan_done && !wifi_count && !wifi_error) text(WIFI_FIRST_ROW, "No networks found", MUTED);
    for (uint32_t row = 0; row < WIFI_ROWS && wifi_page * WIFI_ROWS + row < wifi_count; row++) {
        const NetScan* entry = &networks[wifi_page * WIFI_ROWS + row];
        char label[48];
        memcpy(label, entry->ssid, entry->ssid_length);
        const char* kind = entry->security == NET_SECURITY_OPEN ? "  (Open)" :
                           supported_network(entry->security) ? "  (WPA2)" : "  (Unavailable)";
        uint32_t end = entry->ssid_length;
        do { label[end++] = *kind; } while (*kind++);
        text(WIFI_FIRST_ROW + row * 2, label, TEXT);
    }
}

static void poll_bluetooth(uint32_t now) {
    BtStatus status = { .state = BT_DISABLED };
    if (sys_bt_status(&status) <= 0) status = (BtStatus){ .state = BT_DISABLED };
    if (status.state != bluetooth_status.state || status.count != bluetooth_status.count ||
        status.state == BT_SCANNING) dirty = 1;
    bluetooth_status = status;
    bluetooth_polled = now;
}

static void bluetooth_device_text(const BtDevice* device, char* detail) {
    const char* hex = "0123456789ABCDEF";
    for (uint32_t i = sizeof(device->address); i > 0; i--) {
        uint8_t byte = device->address[i - 1];
        *detail++ = hex[byte >> 4];
        *detail++ = hex[byte & 15];
        if (i > 1) *detail++ = ':';
    }
    if (device->rssi == 127) {
        memcpy(detail, "  RSSI unavailable", sizeof("  RSSI unavailable"));
        return;
    }
    *detail++ = ' ';
    *detail++ = ' ';
    uint32_t magnitude = (uint32_t)device->rssi, divisor = 1;
    if (device->rssi < 0) {
        *detail++ = '-';
        magnitude = 0U - magnitude;
    }
    while (magnitude / divisor >= 10) divisor *= 10;
    do {
        *detail++ = (char)('0' + magnitude / divisor % 10);
        divisor /= 10;
    } while (divisor);
    memcpy(detail, " dBm", sizeof(" dBm"));
}

static void draw_bluetooth(void) {
    text(7, "Bluetooth", TEXT);
    switch (bluetooth_status.state) {
        case BT_BOOTING: text(9, "Starting Bluetooth...", MUTED); break;
        case BT_READY: text(9, "Ready to scan", TEXT); break;
        case BT_SCANNING: text(9, "Scanning for BLE devices...", TEXT); break;
        case BT_FAILED: text(9, "Bluetooth failed", MUTED); break;
        default: text(9, "Bluetooth unavailable", MUTED); break;
    }
    if (bluetooth_scan_failed) text(10, "Could not start scan", MUTED);
    uint32_t shown = 0;
    for (uint32_t i = 0; i < BT_DEVICE_LIMIT && i < bluetooth_status.count; i++) {
        BtDevice device = {0};
        if (sys_bt_device(i, &device) <= 0) continue;
        char name[sizeof(device.name) + 1];
        char detail[sizeof("00:00:00:00:00:00  RSSI unavailable")];
        memcpy(name, device.name, sizeof(device.name));
        name[sizeof(device.name)] = '\0';
        bluetooth_device_text(&device, detail);
        uint32_t row = BT_FIRST_DEVICE_ROW + shown * 2;
        text(row, name[0] ? name : "Unnamed device", TEXT);
        text(row + 1, detail, MUTED);
        shown++;
    }
    if (!shown && bluetooth_status.state == BT_READY) text(BT_FIRST_DEVICE_ROW, "No BLE devices found", MUTED);
}

static void draw(void) {
    sys_canvas_set_color(BG);
    sys_canvas_clear();
    if (view == FILES) draw_files();
    else if (view == USB_FILES) draw_usb_files();
    else if (view == USB_PREVIEW) draw_preview();
    else if (view == NETWORK) draw_network();
    else if (view == WIFI_PASSWORD) draw_password();
    else if (view == BLUETOOTH) draw_bluetooth();
    else { text(1, "Vantage OS", TEXT); draw_clock(); }
    draw_controls();
    sys_canvas_present();
    dirty = 0;
}

static void activate(uint32_t action) {
    if (!action || !enabled(action)) return;
    if (view == USB_FILES && action >= USB_ITEM && action < USB_ITEM + USB_ROWS) {
        read_preview(usb_page * USB_ROWS + action - USB_ITEM, 0, usb_generation);
        view = USB_PREVIEW;
    } else if (view == NETWORK && action >= WIFI_ITEM && action < WIFI_ITEM + WIFI_ROWS) {
        select_network(wifi_page * WIFI_ROWS + action - WIFI_ITEM);
    }
    switch (action) {
        case OPEN_TERMINAL: view = TERMINAL; terminal_app_open(); break;
        case OPEN_FILES: view = FILES; file_page = 0; break;
        case OPEN_USB: view = USB_FILES; usb_page = 0; poll_usb(); break;
        case USB_BACK: view = USB_FILES; poll_usb(); break;
        case USB_PREVIOUS: usb_page--; break;
        case USB_MORE: usb_page++; break;
        case PREVIEW_MORE:
            if (preview_next < preview.length) preview_pos = preview_next;
            else read_preview(preview.index, preview.offset + preview.length, preview.generation);
            break;
        case OPEN_NETWORK: view = NETWORK; poll_network(); break;
        case OPEN_BLUETOOTH:
            view = BLUETOOTH;
            bluetooth_scan_failed = 0;
            poll_bluetooth(sys_time_us());
            break;
        case BACK: view = HOME; break;
        case PREVIOUS: file_page--; break;
        case MORE: file_page++; break;
        case REFRESH: poll_network(); break;
        case WIFI_SCAN: {
            NetScan scan = { .operation = NET_SCAN_START };
            wifi_error = sys_net_scan(&scan);
            wifi_count = wifi_page = wifi_scan_done = 0;
            wifi_scanning = wifi_error == 0;
            break;
        }
        case WIFI_PREVIOUS: wifi_page--; break;
        case WIFI_MORE: wifi_page++; break;
        case WIFI_CANCEL: clear_password(); view = NETWORK; break;
        case WIFI_JOIN: join_network(); break;
        case SCAN:
            bluetooth_scan_failed = sys_bt_scan_start() <= 0;
            poll_bluetooth(sys_time_us());
            break;
    }
    contact_id = TOUCH_CONTACTS_MAX;
    pressed = NONE;
    dirty = 1;
}

static uint32_t hit(const TouchEvent* event) {
    if (event->x < 0 || event->y < 0) return NONE;
    uint32_t x = (uint32_t)event->x, y = (uint32_t)event->y;
    for (const Control* control = controls(); control->action; control++) {
        if (enabled(control->action) && x >= control->x && x - control->x < control->width &&
            y >= control->y && y - control->y < control->height) return control->action;
    }
    if (x >= 24 && x < 456) {
        if (view == USB_FILES && usb_state == USB_MEDIA_READY && y >= USB_FIRST_ROW * TEXT_CELL_HEIGHT) {
            uint32_t row = (y - USB_FIRST_ROW * TEXT_CELL_HEIGHT) / (2U * TEXT_CELL_HEIGHT);
            if (row < USB_ROWS && usb_page * USB_ROWS + row < usb_count) return USB_ITEM + row;
        }
        if (view == NETWORK && !wifi_scanning && y >= WIFI_FIRST_ROW * TEXT_CELL_HEIGHT) {
            uint32_t row = (y - WIFI_FIRST_ROW * TEXT_CELL_HEIGHT) / (2U * TEXT_CELL_HEIGHT);
            if (row < WIFI_ROWS && wifi_page * WIFI_ROWS + row < wifi_count) return WIFI_ITEM + row;
        }
    }
    return NONE;
}

static void password_key(const KeyEvent* key) {
    if (key->type == KEY_HOME || key->type == KEY_CANCEL) activate(WIFI_CANCEL);
    else if (key->type == KEY_ENTER) activate(WIFI_JOIN);
    else if (key->type == KEY_BACKSPACE && password_length) {
        password[--password_length] = 0;
        dirty = 1;
    } else if (key->type == KEY_TEXT && key->character >= 32 && key->character <= 126 &&
               password_length < NET_PASSPHRASE_MAX) {
        password[password_length++] = (char)key->character;
        password[password_length] = 0;
        dirty = 1;
    }
}

static void handle_touch(const TouchContact* contact) {
    const TouchEvent* event = &contact->event;
    uint32_t action = hit(event);
    if (view == WIFI_PASSWORD) {
        KeyEvent key;
        if (keyboard_touch(contact, &key)) password_key(&key);
        if (view != WIFI_PASSWORD) return;
    }
    if (event->type == TOUCH_EVENT_DOWN && contact_id == TOUCH_CONTACTS_MAX && action) {
        contact_id = contact->id;
        pressed = action;
        dirty = 1;
    } else if (contact->id == contact_id) {
        if (event->type == TOUCH_EVENT_MOVE && action != pressed) { pressed = NONE; dirty = 1; }
        if (event->type == TOUCH_EVENT_UP) {
            if (action == pressed) activate(action);
            contact_id = TOUCH_CONTACTS_MAX;
            pressed = NONE;
            dirty = 1;
        }
    }
}

void home_app_init(void) {
    terminal_net_startup();
    view = HOME;
    dirty = 1;
    contact_id = TOUCH_CONTACTS_MAX;
    pressed = NONE;
    clear_password();
    wifi_count = wifi_page = wifi_scanning = wifi_scan_done = 0;
    wifi_error = 0;
    poll_clock();
}

static void step(int refresh_network, int bluetooth_changed, int usb_changed) {
    if (view == HOME && refresh_network) poll_clock();
    if (view == NETWORK && refresh_network) { poll_network(); fetch_networks(); }
    if ((view == USB_FILES || view == USB_PREVIEW) && usb_changed) poll_usb();
    if (view == BLUETOOTH) {
        uint32_t now = sys_time_us();
        if (bluetooth_changed || now - bluetooth_polled >= BT_POLL_US) poll_bluetooth(now);
        if (bluetooth_changed) dirty = 1;
    }
    if (view != TERMINAL) {
        KeyEvent key;
        TouchContact contact;
        for (uint32_t i = 0; i < 64 && view != TERMINAL && sys_key_read(&key); i++) {
            if (view == WIFI_PASSWORD) password_key(&key);
            else if (view == USB_PREVIEW && (key.type == KEY_CANCEL || key.type == KEY_HOME)) activate(USB_BACK);
            else if (key.type == KEY_CANCEL || key.type == KEY_HOME) activate(BACK);
            else if (view == HOME && (key.type == KEY_ENTER ||
                     (key.type == KEY_TEXT && key.character == '1'))) activate(OPEN_TERMINAL);
            else if (view == HOME && key.type == KEY_TEXT && key.character == '2') activate(OPEN_FILES);
            else if (view == HOME && key.type == KEY_TEXT && key.character == '3') activate(OPEN_NETWORK);
            else if (view == HOME && key.type == KEY_TEXT && key.character == '4') activate(OPEN_BLUETOOTH);
            else if (view == BLUETOOTH && key.type == KEY_ENTER) activate(SCAN);
            else if (view == FILES && key.type == KEY_TEXT && key.character == 'u') activate(OPEN_USB);
            else if (view == NETWORK && key.type == KEY_TEXT && key.character == 's') activate(WIFI_SCAN);
        }
        for (uint32_t i = 0; i < 64 && view != TERMINAL && sys_touch_contact_read(&contact); i++) {
            if (contact.id < TOUCH_CONTACTS_MAX) handle_touch(&contact);
        }
    }
    if (view == TERMINAL) {
        terminal_app_step();
        if (terminal_app_closed()) activate(BACK);
    }
    if (view == WIFI_PASSWORD) {
        KeyEvent key;
        if (keyboard_repeat(&key)) password_key(&key);
    }
    if (view != TERMINAL && dirty) draw();
    else if (view == WIFI_PASSWORD && keyboard_draw()) sys_canvas_present();
}

void home_app_step(void) {
    step(1, 0, 1);
}

void user_task(void) {
    enum { FRAME_US = 16667 };
    home_app_init();
    int ready = 0;
    for (;;) {
        /* A timeout also resamples status if the view opened before its first wait. */
        step(!ready || (ready & EVENT_NETWORK), ready & EVENT_BLUETOOTH, !ready || (ready & EVENT_USB));

        uint32_t interests = EVENT_KEY | EVENT_TOUCH;
        uint32_t timeout = EVENT_WAIT_MAX_US;
        if (view == TERMINAL && terminal_app_needs_frame()) timeout = FRAME_US;
        else if (view == NETWORK || view == HOME) interests |= EVENT_NETWORK;
        else if (view == USB_FILES || view == USB_PREVIEW) interests |= EVENT_USB;
        else if (view == WIFI_PASSWORD && keyboard_repeat_pending()) timeout = FRAME_US;
        else if (view == BLUETOOTH) {
            interests |= EVENT_BLUETOOTH;
            if (bluetooth_status.state == BT_BOOTING || bluetooth_status.state == BT_SCANNING)
                timeout = BT_POLL_US;
        }

        ready = sys_event_wait(interests, timeout);
        if (ready < 0) {
            static const char message[] = "[ERR] Home event wait failed\n";
            sys_console_write(message, sizeof(message) - 1);
            sys_task_exit(ready);
            return;
        }
    }
}
