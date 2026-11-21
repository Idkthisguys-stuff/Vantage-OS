#include "home.h"
#include "terminal.h"
#include <vantage/user.h>

#define BG RGB(13, 24, 29)
#define TEXT RGB(232, 245, 242)
#define MUTED RGB(156, 188, 185)
#define TILE RGB(30, 61, 67)
#define ACTIVE RGB(38, 103, 100)

enum { HOME, TERMINAL, FILES, NETWORK };
enum { NONE, OPEN_TERMINAL, OPEN_FILES, OPEN_NETWORK, BACK, PREVIOUS, MORE, REFRESH };
enum { FILE_ROWS = 12, SCAN_BUDGET = FILE_ROWS + 1 };
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
    {NONE, 0, 0, 0, 0, ""}
};
static const Control file_controls[] = {
    {BACK, 24, 32, 90, 40, "Home"},
    {PREVIOUS, 24, 704, 132, 48, "Prev"},
    {MORE, 324, 704, 132, 48, "Next"},
    {NONE, 0, 0, 0, 0, ""}
};
static const Control network_controls[] = {
    {BACK, 24, 32, 90, 40, "Home"},
    {REFRESH, 24, 288, 132, 48, "Refresh"},
    {NONE, 0, 0, 0, 0, ""}
};

static uint32_t view, dirty, pressed, contact_id;
static uint32_t file_page, file_more;

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
    if (view == NETWORK) return network_controls;
    return home_controls;
}

static uint32_t enabled(uint32_t action) {
    return (action != PREVIOUS || file_page) && (action != MORE || file_more);
}

static void draw_controls(void) {
    for (const Control* control = controls(); control->action; control++) {
        if (!enabled(control->action)) continue;
        CanvasButton button = { .x = control->x, .y = control->y,
                                .width = control->width, .height = control->height,
                                .radius = 10, .foreground = TEXT,
                                .background = control->action == pressed ? ACTIVE : TILE };
        for (uint32_t i = 0; control->label[i]; i++) button.label[i] = control->label[i];
        sys_canvas_button(&button);
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

static void draw_network(void) {
    text(7, "Network", TEXT);
    NetStatus status = {0};
    sys_net_status(&status);

    if (!status.available) text(9, "Wi-Fi unavailable", MUTED);
    else if (status.link == NET_LINK_UP) {
        text(9, "Wi-Fi connected", TEXT);
        status.ssid[NET_SSID_MAX] = '\0';
        text(10, status.ssid, MUTED);
    } else text(9, "Wi-Fi disconnected", MUTED);

    text(12, status.ethernet == NET_ETHERNET_UP ? "Ethernet connected" :
             status.ethernet == NET_ETHERNET_NO_CABLE ? "Ethernet cable unplugged" :
             "Ethernet unavailable", MUTED);

    if (status.configuration == NET_ADDRESS_BOUND) {
        char address[16];
        address_text(status.address, address);
        text(14, "IP address", TEXT);
        text(15, address, MUTED);
    } else text(14, "No IP address", MUTED);
}

static void draw(void) {
    sys_canvas_set_color(BG);
    sys_canvas_clear();
    if (view == FILES) draw_files();
    else if (view == NETWORK) draw_network();
    else text(1, "Vantage OS", TEXT);
    draw_controls();
    sys_canvas_present();
    dirty = 0;
}

static void activate(uint32_t action) {
    if (!action || !enabled(action)) return;
    switch (action) {
        case OPEN_TERMINAL: view = TERMINAL; terminal_app_open(); break;
        case OPEN_FILES: view = FILES; file_page = 0; break;
        case OPEN_NETWORK: view = NETWORK; break;
        case BACK: view = HOME; break;
        case PREVIOUS: file_page--; break;
        case MORE: file_page++; break;
        case REFRESH: break;
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
    return NONE;
}

static void handle_touch(const TouchContact* contact) {
    const TouchEvent* event = &contact->event;
    uint32_t action = hit(event);
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
}

void home_app_step(void) {
    if (view == TERMINAL) {
        terminal_app_step();
        if (terminal_app_closed()) activate(BACK);
    } else {
        KeyEvent key;
        TouchContact contact;
        for (uint32_t i = 0; i < 64 && view != TERMINAL && sys_key_read(&key); i++) {
            if (key.type == KEY_CANCEL || key.type == KEY_HOME) activate(BACK);
            else if (view == HOME && (key.type == KEY_ENTER ||
                     (key.type == KEY_TEXT && key.character == '1'))) activate(OPEN_TERMINAL);
            else if (view == HOME && key.type == KEY_TEXT && key.character == '2') activate(OPEN_FILES);
            else if (view == HOME && key.type == KEY_TEXT && key.character == '3') activate(OPEN_NETWORK);
        }
        for (uint32_t i = 0; i < 64 && view != TERMINAL && sys_touch_contact_read(&contact); i++) {
            if (contact.id < TOUCH_CONTACTS_MAX) handle_touch(&contact);
        }
    }
    if (view != TERMINAL && dirty) draw();
}

void user_task(void) {
    home_app_init();
    for (;;) {
        home_app_step();
        sys_frame_wait();
    }
}
