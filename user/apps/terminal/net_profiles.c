#include "net.h"

/* Saved Wi-Fi networks, pushed to the kernel at startup. Text format: the
 * header line, then one "FLAGS SSID PASSPHRASE" line per network with FLAGS
 * 0 or 1 (hidden), SSID and passphrase as lowercase hex, and "-" for no
 * passphrase. Malformed lines are ignored. */
#define PROFILES_PATH "/ram/.netprofiles"
#define PROFILES_HEADER "vantage-networks 1"
#define PROFILES_LINE (2U + NET_SSID_MAX * 2U + 1U + NET_PASSPHRASE_MAX * 2U)

typedef struct {
    uint32_t flags, ssid_length, passphrase_length;
    char ssid[NET_SSID_MAX], passphrase[NET_PASSPHRASE_MAX];
} Saved;
typedef struct {
    uint32_t count;
    Saved entry[NET_PROFILE_LIMIT];
} SavedList;

static int hex_value(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* Decodes hex up to a space or the end; returns the byte count or 0. */
static uint32_t hex_decode(const char** text, const char* end, char* out, uint32_t capacity) {
    uint32_t count = 0;
    const char* p = *text;
    while (p < end && *p != ' ') {
        int high = hex_value(p[0]), low = p + 1 < end ? hex_value(p[1]) : -1;
        if (high < 0 || low < 0 || count == capacity) return 0;
        out[count++] = (char)(high << 4 | low);
        p += 2;
    }
    *text = p;
    return count;
}

static void add(SavedList* list, const Saved* entry) {
    uint32_t i = 0;
    while (i < list->count && (list->entry[i].ssid_length != entry->ssid_length ||
                               memcmp(list->entry[i].ssid, entry->ssid, entry->ssid_length))) i++;
    if (i == NET_PROFILE_LIMIT) return;
    if (i == list->count) list->count++;
    list->entry[i] = *entry;
}

static void parse(SavedList* list, const char* line, uint32_t length) {
    const char *p = line + 2, *end = line + length;
    Saved entry = {0};
    if (length < 5 || (line[0] != '0' && line[0] != '1') || line[1] != ' ') return;
    entry.flags = line[0] == '1' ? NET_PROFILE_HIDDEN : 0;
    entry.ssid_length = hex_decode(&p, end, entry.ssid, NET_SSID_MAX);
    if (entry.ssid_length && p < end && *p++ == ' ') {
        if (end - p == 1 && *p == '-') add(list, &entry);
        else if ((entry.passphrase_length = hex_decode(&p, end, entry.passphrase, NET_PASSPHRASE_MAX)) >=
                 NET_PASSPHRASE_MIN && p == end) {
            uint32_t i = 0;
            while (i < entry.passphrase_length && entry.passphrase[i] >= 32 && entry.passphrase[i] <= 126) i++;
            if (i == entry.passphrase_length) add(list, &entry);
        }
    }
    terminal_erase(&entry, sizeof(entry));
}

/* 0 with the list (empty when the file is missing or not ours), or a FileError. */
static int load(SavedList* list) {
    char line[PROFILES_LINE + 1];
    unsigned char chunk[FILE_IO_MAX];
    uint32_t length = 0, lines = 0, overflow = 0, foreign = 0;
    int result = 0;
    list->count = 0;
    int64_t handle = sys_file_open(PROFILES_PATH, FILE_READ);
    if (handle == FILE_NOT_FOUND) return 0;
    if (handle < 0) return (int)handle;
    while (!foreign && (result = sys_file_read((uint64_t)handle, chunk, sizeof(chunk))) > 0) {
        for (int i = 0; i < result && !foreign; i++) {
            if (chunk[i] != '\n') {
                if (length < sizeof(line)) line[length++] = (char)chunk[i]; else overflow = 1;
                continue;
            }
            if (!lines++) foreign = overflow || length != sizeof(PROFILES_HEADER) - 1 || memcmp(line, PROFILES_HEADER, length);
            else if (!overflow) parse(list, line, length);
            length = overflow = 0;
        }
    }
    int closed = sys_file_close((uint64_t)handle);
    terminal_erase(line, sizeof(line));
    terminal_erase(chunk, sizeof(chunk));
    if (foreign || result < 0 || closed < 0) terminal_erase(list, sizeof(*list));
    return result < 0 ? result : closed < 0 ? closed : 0;
}

typedef struct {
    uint64_t handle;
    uint32_t used;
    int error;
    unsigned char data[FILE_IO_MAX];
} Writer;

static void flush(Writer* w) {
    int result = w->error || !w->used ? 0 : sys_file_write(w->handle, w->data, w->used);
    if (result != (int)w->used && !w->error) w->error = result < 0 ? result : FILE_IO_ERROR;
    w->used = 0;
}

static void emit(Writer* w, const char* text, uint32_t length, int hex) {
    for (uint32_t i = 0; i < (hex ? length * 2U : length); i++) {
        unsigned char c = (unsigned char)text[hex ? i / 2 : i];
        w->data[w->used++] = hex ? (unsigned char)"0123456789abcdef"[i & 1 ? c & 15 : c >> 4] : c;
        if (w->used == FILE_IO_MAX) flush(w);
    }
}

static int save(const SavedList* list) {
    Writer w;
    int64_t handle = sys_file_open(PROFILES_PATH, FILE_WRITE | FILE_CREATE | FILE_TRUNCATE);
    if (handle < 0) return (int)handle;
    w.handle = (uint64_t)handle; w.used = 0; w.error = 0;
    emit(&w, PROFILES_HEADER "\n", sizeof(PROFILES_HEADER), 0);
    for (uint32_t i = 0; i < list->count; i++) {
        const Saved* e = &list->entry[i];
        emit(&w, e->flags & NET_PROFILE_HIDDEN ? "1 " : "0 ", 2, 0);
        emit(&w, e->ssid, e->ssid_length, 1);
        emit(&w, " ", 1, 0);
        if (e->passphrase_length) emit(&w, e->passphrase, e->passphrase_length, 1); else emit(&w, "-", 1, 0);
        emit(&w, "\n", 1, 0);
    }
    flush(&w);
    int closed = sys_file_close(w.handle), error = w.error;
    terminal_erase(&w, sizeof(w));
    return error ? error : closed < 0 ? closed : 0;
}

int net_profiles_store(const char* ssid, uint32_t ssid_length, const char* passphrase, uint32_t flags) {
    SavedList list;
    Saved entry = {0};
    int result = load(&list);
    if (result < 0) return result;
    entry.flags = flags & NET_PROFILE_HIDDEN;
    entry.ssid_length = ssid_length;
    memcpy(entry.ssid, ssid, ssid_length);
    while (passphrase[entry.passphrase_length]) entry.passphrase_length++;
    memcpy(entry.passphrase, passphrase, entry.passphrase_length);
    /* The kernel accepted it, so a full file holds a stale entry: drop the oldest. */
    uint32_t i = 0;
    while (i < list.count && (list.entry[i].ssid_length != ssid_length || memcmp(list.entry[i].ssid, ssid, ssid_length))) i++;
    if (i == NET_PROFILE_LIMIT) {
        for (i = 1; i < NET_PROFILE_LIMIT; i++) list.entry[i - 1] = list.entry[i];
        list.count--;
    }
    add(&list, &entry);
    result = save(&list);
    terminal_erase(&entry, sizeof(entry));
    terminal_erase(&list, sizeof(list));
    return result;
}

int net_profiles_remove(const char* ssid, uint32_t ssid_length) {
    SavedList list;
    int result = load(&list);
    if (result < 0) return result;
    for (uint32_t i = 0; i < list.count; i++) {
        if (list.entry[i].ssid_length != ssid_length || memcmp(list.entry[i].ssid, ssid, ssid_length)) continue;
        for (list.count--; i < list.count; i++) list.entry[i] = list.entry[i + 1];
        result = save(&list);
        terminal_erase(&list, sizeof(list));
        return result < 0 ? result : 1;
    }
    terminal_erase(&list, sizeof(list));
    return 0;
}

void terminal_net_startup(void) {
    SavedList list;
    NetProfile profile = {0};
    if (load(&list) < 0) return;
    profile.operation = NET_PROFILE_CLEAR;
    if (sys_net_profile(&profile) >= 0) {
        for (uint32_t i = 0; i < list.count; i++) {
            const Saved* e = &list.entry[i];
            memset(&profile, 0, sizeof(profile));
            profile.operation = NET_PROFILE_ADD;
            profile.flags = e->flags;
            profile.ssid_length = e->ssid_length;
            profile.passphrase_length = e->passphrase_length;
            memcpy(profile.ssid, e->ssid, e->ssid_length);
            memcpy(profile.passphrase, e->passphrase, e->passphrase_length);
            (void)sys_net_profile(&profile);
        }
    }
    terminal_erase(&profile, sizeof(profile));
    terminal_erase(&list, sizeof(list));
}
