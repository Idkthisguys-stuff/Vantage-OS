#include "user/apps/terminal/terminal.h"
#include <vantage/abi/syscall.h>
#include <vantage/tls.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n%s\n", __LINE__, #c, transcript + mark); exit(1); } } while (0)

static struct { unsigned char before[64]; Terminal terminal; unsigned char after[64]; } guarded;
static char transcript[262144];
static size_t written, mark;
static uint32_t info_kind = CONSOLE_MEMORY_KERNEL;
static uint32_t clock_now;

/* Scripted kernel: networking follows the state below; files live in memory. */
static NetStatus net_state;
static NetTime net_clock;
static int join_result, profile_result, resolve_result = NET_UNAVAILABLE, connect_again, receive_error;
static unsigned int joins, leaves, clears, aborts, closes;
static uint32_t joined_flags, resolve_address = 0xc0a8320aU;
static char joined_ssid[NET_SSID_MAX + 1], joined_passphrase[64];

typedef struct { uint32_t flags, ssid_length, passphrase_length; char ssid[NET_SSID_MAX], passphrase[64]; } Profile;
static Profile profiles[NET_PROFILE_LIMIT];
static uint32_t profile_count;

static NetScan scan_entries[3];
static uint32_t scan_count, scan_pending;

static const char* response;
static size_t response_length, response_at;
static uint32_t response_paused;
static char request_text[2048];
static size_t request_length;
static uint32_t echo_pending, echo_replies;

static struct { char path[FILE_PATH_MAX]; unsigned char data[8192]; uint32_t size, used; } files[4];
static uint32_t storage_mounted, file_capacity = sizeof(files[0].data), open_file = ~0U, read_offset;

static int find_file(const char* path) {
    for (int i = 0; i < 4; i++) if (files[i].used && !strcmp(files[i].path, path)) return i;
    return -1;
}

static int64_t file_call(uint64_t id, uint64_t argument) {
    if (!storage_mounted) return FILE_UNAVAILABLE;
    if (id == SYS_FILE_OPEN_LONG) {
        const FileOpenLong* r = (const FileOpenLong*)(uintptr_t)argument;
        int i = find_file(r->path);
        if (i < 0 && !(r->flags & FILE_CREATE)) return FILE_NOT_FOUND;
        if (i < 0) {
            for (i = 0; i < 4 && files[i].used; i++) {}
            CHECK(i < 4);
            memset(&files[i], 0, sizeof(files[i]));
            snprintf(files[i].path, sizeof(files[i].path), "%s", r->path); files[i].used = 1;
        }
        if (r->flags & FILE_TRUNCATE) files[i].size = 0;
        open_file = (uint32_t)i;
        read_offset = 0;
        return 0x400 + i;
    }
    if (id == SYS_FILE_CLOSE) { CHECK(argument == 0x400 + open_file); open_file = ~0U; return 0; }
    if (id == SYS_FILE_REMOVE_LONG) {
        int i = find_file(((const FilePathLong*)(uintptr_t)argument)->path);
        if (i < 0) return FILE_NOT_FOUND;
        files[i].used = 0;
        return 0;
    }
    if (id == SYS_FILE_READ || id == SYS_FILE_WRITE) {
        FileIo* io = (FileIo*)(uintptr_t)argument;
        CHECK(io->handle == 0x400 + open_file);
        if (id == SYS_FILE_WRITE) {
            if (files[open_file].size + io->length > file_capacity) return FILE_NO_SPACE;
            memcpy(files[open_file].data + files[open_file].size, io->data, io->length);
            files[open_file].size += io->length;
            return (int64_t)io->length;
        }
        uint32_t left = files[open_file].size - read_offset, count = left < io->length ? left : io->length;
        memcpy(io->data, files[open_file].data + read_offset, count);
        read_offset += count;
        return count;
    }
    return FILE_BAD;
}

static int64_t profile_call(NetProfile* p) {
    if (profile_result) return profile_result;
    if (p->operation == NET_PROFILE_CLEAR) { clears++; profile_count = 0; return 0; }
    if (p->operation == NET_PROFILE_LIST) {
        if (p->cursor >= profile_count) return 0;
        const Profile* e = &profiles[p->cursor++];
        p->flags = e->flags; p->ssid_length = e->ssid_length; p->passphrase_length = e->passphrase_length != 0;
        memcpy(p->ssid, e->ssid, e->ssid_length);
        return 1;
    }
    uint32_t i = 0;
    while (i < profile_count && (profiles[i].ssid_length != p->ssid_length || memcmp(profiles[i].ssid, p->ssid, p->ssid_length))) i++;
    if (p->operation == NET_PROFILE_REMOVE) {
        if (i == profile_count) return NET_NOT_FOUND;
        profiles[i] = profiles[--profile_count];
        return 0;
    }
    if (i == NET_PROFILE_LIMIT) return NET_NO_SPACE;
    if (i == profile_count) profile_count++;
    memset(&profiles[i], 0, sizeof(profiles[i]));
    profiles[i].flags = p->flags; profiles[i].ssid_length = p->ssid_length; profiles[i].passphrase_length = p->passphrase_length;
    memcpy(profiles[i].ssid, p->ssid, p->ssid_length);
    memcpy(profiles[i].passphrase, p->passphrase, p->passphrase_length);
    return 0;
}

static int64_t socket_call(NetSocketCall* c) {
    if (c->operation == NET_SOCKET_OPEN) return c->type == NET_ICMP ? 0x202 : 0x101;
    if (c->operation == NET_SOCKET_CONNECT) return c->handle == 0x101 && connect_again && connect_again-- ? NET_AGAIN : 0;
    if (c->operation == NET_SOCKET_CLOSE) { closes++; if (c->flags & NET_CLOSE_ABORT) aborts++; return 0; }
    if (c->operation == NET_SOCKET_SEND) {
        if (c->handle == 0x202) { echo_pending = c->port; return c->length; }
        CHECK(request_length + c->length < sizeof(request_text));
        memcpy(request_text + request_length, c->data, c->length);
        request_length += c->length;
        return c->length;
    }
    if (c->operation == NET_SOCKET_RECEIVE) {
        if (c->handle == 0x202) {
            if (!echo_pending || !echo_replies) return NET_AGAIN;
            c->port = (uint16_t)echo_pending; c->address = resolve_address; echo_pending = 0;
            return 56;
        }
        if (response_paused) return NET_AGAIN;
        if (response_at == response_length) return receive_error;
        size_t count = response_length - response_at < 300 ? response_length - response_at : 300;
        if (count > c->length) count = c->length;
        memcpy(c->data, response + response_at, count);
        response_at += count;
        return (int64_t)count;
    }
    return NET_BAD;
}

uint64_t handle_sys_call(uint64_t id, uint64_t argument) {
    if (id == SYS_TIME_US) return clock_now;
    if (id == SYS_NET_STATUS) { memcpy((void*)(uintptr_t)argument, &net_state, sizeof(net_state)); return 0; }
    if (id == SYS_NET_TIME) { memcpy((void*)(uintptr_t)argument, &net_clock, sizeof(net_clock)); return net_clock.synced; }
    if (id == SYS_NET_JOIN) {
        const NetJoin* j = (const NetJoin*)(uintptr_t)argument;
        CHECK(j->ssid_length <= NET_SSID_MAX && j->passphrase_length <= NET_PASSPHRASE_MAX);
        memset(joined_ssid, 0, sizeof(joined_ssid)); memset(joined_passphrase, 0, sizeof(joined_passphrase));
        memcpy(joined_ssid, j->ssid, j->ssid_length);
        memcpy(joined_passphrase, j->passphrase, j->passphrase_length);
        joined_flags = j->flags;
        joins++;
        return (uint64_t)(int64_t)join_result;
    }
    if (id == SYS_NET_LEAVE) { leaves++; return 0; }
    if (id == SYS_NET_PROFILE) return (uint64_t)profile_call((NetProfile*)(uintptr_t)argument);
    if (id == SYS_NET_RESOLVE) {
        NetResolve* r = (NetResolve*)(uintptr_t)argument;
        if (resolve_result > 0) r->address = resolve_address;
        return (uint64_t)(int64_t)resolve_result;
    }
    if (id == SYS_NET_SCAN) {
        NetScan* s = (NetScan*)(uintptr_t)argument;
        if (!net_state.available) return (uint64_t)(int64_t)NET_UNAVAILABLE;
        if (s->operation == NET_SCAN_START) return 0;
        if (scan_pending) { scan_pending--; return (uint64_t)(int64_t)NET_AGAIN; }
        if (s->cursor >= scan_count) return 0;
        uint32_t cursor = s->cursor;
        *s = scan_entries[cursor]; s->operation = NET_SCAN_RESULT; s->cursor = cursor + 1;
        return 1;
    }
    if (id == SYS_NET_SOCKET) return (uint64_t)socket_call((NetSocketCall*)(uintptr_t)argument);
    if (id >= SYS_NET_STATUS && id <= SYS_NET_RANDOM) return (uint64_t)(int64_t)NET_UNAVAILABLE;
    return (uint64_t)file_call(id, argument);
}

/* Scripted TLS session over the scripted socket. */
static int tls_open_result, tls_handshakes, tls_handshake_result = 1, tls_receive_error, tls_closes;
static char tls_host[NET_NAME_MAX];
int tls_open(uint64_t socket, const char* host) {
    CHECK(socket == 0x101);
    snprintf(tls_host, sizeof(tls_host), "%s", host);
    return tls_open_result;
}
int tls_handshake(void) { return tls_handshakes ? (tls_handshakes--, NET_AGAIN) : tls_handshake_result; }
int tls_send(const void* data, uint32_t length) {
    uint32_t count = length < 10 ? length : 10;
    memcpy(request_text + request_length, data, count);
    request_length += count;
    return (int)count;
}
int tls_receive(void* data, uint32_t capacity) {
    if (response_at == response_length) return tls_receive_error;
    size_t count = response_length - response_at < capacity ? response_length - response_at : capacity;
    memcpy(data, response + response_at, count);
    response_at += count;
    return (int)count;
}
void tls_close(void) { tls_closes++; }
const char* tls_error_text(int error) { return error == TLS_CERTIFICATE ? "certificate rejected" : "no trusted certificates"; }

static void output(void* context, char c) {
    CHECK(context == &guarded);
    if (written + 1 < sizeof(transcript)) {
        transcript[written++] = c;
        transcript[written] = 0;
    }
}

static uint32_t info(void* context, ConsoleInfo* out) {
    CHECK(context == &guarded);
    memset(out, 0, sizeof(*out));
    out->memory_kind = info_kind;
    out->host_backbuffer_bytes = 1536000;
    out->memory.ram_bytes = 128 * 1024 * 1024;
    out->memory.free_pages = 123;
    out->memory.allocatable_pages = 456;
    out->memory.heap_used_bytes = ~(uint64_t)0;
    out->memory.heap_bytes = 8 * 1024 * 1024;
    return 1;
}

static void reset(uint32_t cols, uint32_t rows) {
    memset(&guarded, 0xa7, sizeof(guarded));
    written = 0; transcript[0] = 0;
    CHECK(terminal_init(&guarded.terminal, cols, rows, info, output, &guarded));
}

static void key(uint32_t type, uint32_t character) {
    KeyEvent event = {type, character};
    terminal_key(&guarded.terminal, &event);
}

static void text(const char* string) {
    while (*string) key(KEY_TEXT, (unsigned char)*string++);
}

static void command(const char* string) {
    text(string);
    key(KEY_ENTER, 0);
}

static void guards(void) {
    size_t i;
    for (i = 0; i < 64; i++) CHECK(guarded.before[i] == 0xa7 && guarded.after[i] == 0xa7);
    CHECK(guarded.terminal.row < guarded.terminal.rows);
    CHECK(guarded.terminal.column <= guarded.terminal.columns);
    CHECK(guarded.terminal.length <= guarded.terminal.input_limit);
    CHECK(!guarded.terminal.command[guarded.terminal.length]);
}

static int seen(const char* expected) { return strstr(transcript + mark, expected) != NULL; }
static void since(void) { mark = written; }

static void run(void) {
    for (uint32_t i = 0; i < 64 && guarded.terminal.job.type; i++) terminal_job_step(&guarded.terminal);
    CHECK(!guarded.terminal.job.type);
}

static int state_clear(void) {
    const unsigned char* p = (const unsigned char*)guarded.terminal.job.state;
    for (size_t i = 0; i < sizeof(guarded.terminal.job.state); i++) if (p[i]) return 0;
    return 1;
}

static void serve(const char* text_response) {
    response = text_response; response_length = strlen(text_response); response_at = 0;
    request_length = 0; memset(request_text, 0, sizeof(request_text));
}

static void store_file(const char* contents) {
    memset(files, 0, sizeof(files));
    snprintf(files[0].path, sizeof(files[0].path), "/ram/.netprofiles"); files[0].used = 1;
    files[0].size = (uint32_t)strlen(contents);
    memcpy(files[0].data, contents, files[0].size);
}

static const char* file_text(const char* path) {
    static char copy[8193];
    int i = find_file(path);
    if (i < 0) return NULL;
    memcpy(copy, files[i].data, files[i].size); copy[files[i].size] = 0;
    return copy;
}

static void join_network(const char* line, const char* passphrase) {
    net_state.link = NET_LINK_JOINING; net_state.configuration = NET_ADDRESS_NONE;
    command(line);
    if (passphrase) { text(passphrase); key(KEY_ENTER, 0); }
    CHECK(guarded.terminal.job.type);
    net_state.link = NET_LINK_UP; net_state.configuration = NET_ADDRESS_BOUND;
    run();
}

static void network_tests(void) {
    Terminal* t = &guarded.terminal;
    uint32_t i;
    reset(60, 50);
    since(); command("net");
    CHECK(seen("Not connected.\nWi-Fi: unavailable (no radio firmware)\nTime: not synced\n> "));
    since(); command("help");
    CHECK(seen("net    Network and Wi-Fi ('net help')\n") && !seen("wifi") && !seen("http"));
    since(); command("net help");
    CHECK(seen("net                   Connection status\n") && seen("net join NAME         Join Wi-Fi (--open, --hidden)\n"));
    CHECK(seen("net get URL [-o FILE] Fetch a web page\n") && !seen("net help "));
    static const char* usage[] = {
        "net bogus", "net join", "net join \"unterminated", "net join \"a\"b", "net join a b",
        "net join 123456789012345678901234567890123", "net join --bogus a", "net join --open",
        "net scan now", "net leave x", "net saved x", "net help me", "net forget", "net forget a b",
        "net ping", "net ping host 0", "net ping host 101", "net ping host x", "net ping a 1 2",
        "net lookup", "net lookup a b", "net get", "net get ftp://x", "net get http://", "net get http://h:99999/",
        "net get http://h:0/", "net get http://h/ -o", "net get http://a http://b", "net get http://h/ -o a -o b",
        "net verylongword", "wifi", "ping host", "dns host", "http http://host/"};
    for (i = 0; i < sizeof(usage) / sizeof(usage[0]); i++) {
        since(); command(usage[i]);
        CHECK(!t->job.type && !t->secret && !joins);
        CHECK(seen("Usage: net ") || seen("Unknown net command. Type 'net help'.") || seen("Unknown command."));
        CHECK(state_clear());
    }
    since(); command("net get http://h/ -o ../name"); CHECK(seen("Invalid file name.") && !t->job.type);

    since(); command("net lookup vantage.test");
    CHECK(t->job.type);
    key(KEY_TEXT, 'x'); CHECK(!t->length);            /* Input is held while a job runs. */
    terminal_job_step(t);
    CHECK(!t->job.type && seen("vantage.test: Network unavailable.\n> "));
    resolve_result = NET_AGAIN;
    since(); command("net lookup slow.test");
    terminal_job_step(t); CHECK(t->job.type);
    resolve_result = 1;
    run(); CHECK(seen("slow.test is 192.168.50.10\n> "));
    resolve_result = NET_AGAIN;
    since(); command("net lookup never.test");
    clock_now += 16000000U; run(); CHECK(seen("never.test: Timed out."));
    resolve_result = 1;

    /* Join with a masked passphrase; the option may follow the name. */
    since(); command("net join \"My Home Net\" --hidden");
    CHECK(t->secret && seen("Passphrase: "));
    text("hunter2!x");
    CHECK(seen("Passphrase: *********") && !strstr(transcript, "hunter2"));
    key(KEY_CLEAR, 0);
    CHECK(!strstr(transcript, "hunter2"));
    net_state.available = 1; net_state.link = NET_LINK_JOINING;
    key(KEY_ENTER, 0);
    CHECK(joins == 1 && !strcmp(joined_ssid, "My Home Net") && !strcmp(joined_passphrase, "hunter2!x"));
    CHECK(joined_flags == NET_JOIN_HIDDEN && seen("Joining \"My Home Net\"... (Cancel to stop)\n"));
    for (i = 0; i < TERMINAL_COMMAND_SIZE; i++) CHECK(!t->command[i]);
    CHECK(t->job.type && !t->secret);
    terminal_job_step(t); CHECK(t->job.type);
    net_state.link = NET_LINK_UP; net_state.configuration = NET_ADDRESS_BOUND; net_state.address = 0x0a000005U;
    terminal_job_step(t);
    CHECK(!t->job.type && seen("Connected. Address 10.0.0.5. Saved for this session only (no storage).\n> "));
    CHECK(state_clear() && !strstr(transcript, "hunter2"));
    CHECK(profile_count == 1 && profiles[0].flags == NET_PROFILE_HIDDEN && !memcmp(profiles[0].passphrase, "hunter2!x", 10));

    /* With storage the network persists in the saved-networks file. */
    storage_mounted = 1;
    since(); join_network("net join --open Cafe", NULL);
    CHECK(joins == 2 && !joined_passphrase[0] && !joined_flags);
    CHECK(seen("Connected. Address 10.0.0.5. Saved; joins automatically at boot.\n"));
    CHECK(!strcmp(file_text("/ram/.netprofiles"), "vantage-networks 1\n0 43616665 -\n"));
    join_network("net join Office", "office-pass");
    CHECK(!strcmp(file_text("/ram/.netprofiles"),
                  "vantage-networks 1\n0 43616665 -\n0 4f6666696365 6f66666963652d70617373\n"));
    join_network("net join Office --hidden", "office-pass2");   /* Replaces the entry. */
    CHECK(!strcmp(file_text("/ram/.netprofiles"),
                  "vantage-networks 1\n0 43616665 -\n1 4f6666696365 6f66666963652d7061737332\n"));
    since(); command("net saved");
    CHECK(seen("Saved networks:\n  \"My Home Net\" (hidden)\n  \"Cafe\" (open)\n  \"Office\" (hidden)\n> "));
    CHECK(!strstr(transcript, "office-pass"));

    /* Startup pushes the file to the kernel, replacing what it had. */
    size_t quiet = written;
    profile_count = 0; clears = 0;
    terminal_net_startup();
    CHECK(clears == 1 && profile_count == 2 && written == quiet);
    CHECK(profiles[0].ssid_length == 4 && !profiles[0].passphrase_length && !profiles[0].flags);
    CHECK(profiles[1].flags == NET_PROFILE_HIDDEN && profiles[1].passphrase_length == 12 &&
          !memcmp(profiles[1].passphrase, "office-pass2", 12));
    CHECK(state_clear());

    /* Strict parsing: only well-formed lines load; duplicates replace. */
    char long_line[400]; memset(long_line, 'a', sizeof(long_line)); long_line[399] = 0;
    char contents[4096];
    snprintf(contents, sizeof(contents), "vantage-networks 1\n0 6f6b -\n0 4F4B -\n0 6f6 -\n2 6b -\n1 6b\n"
             "0 6b 31323334353637\n0 6b 0102030405060708\n0 6b 3132333435363738 \n0  -\n%s\n"
             "0 %s -\n1 61 3132333435363738\n0 61 -\n0 6c6173 -", long_line,
             "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20");
    store_file(contents);
    profile_count = 0;
    terminal_net_startup();
    CHECK(profile_count == 2 && profiles[0].ssid_length == 2 && !memcmp(profiles[0].ssid, "ok", 2));
    CHECK(profiles[1].ssid_length == 1 && profiles[1].ssid[0] == 'a' && !profiles[1].passphrase_length && !profiles[1].flags);
    {   /* Any SSID bytes round trip, and at most NET_PROFILE_LIMIT entries are kept. */
        size_t n = (size_t)snprintf(contents, sizeof(contents), "vantage-networks 1\n");
        for (i = 0; i < 10; i++) n += (size_t)snprintf(contents + n, sizeof(contents) - n, "0 00ff%02x -\n", i);
        store_file(contents);
        profile_count = 0; terminal_net_startup();
        CHECK(profile_count == NET_PROFILE_LIMIT && !memcmp(profiles[7].ssid, "\0\xff\x07", 3));
    }
    store_file("vantage-networks 2\n0 6f6b -\n");
    profile_count = 1; clears = 0; terminal_net_startup();
    CHECK(clears == 1 && !profile_count);
    storage_mounted = 0; profile_count = 1; clears = 0; quiet = written;
    terminal_net_startup();
    CHECK(!clears && profile_count == 1 && written == quiet);
    storage_mounted = 1; store_file("vantage-networks 1\n0 43616665 -\n0 4f6666696365 6f66666963652d70617373\n");
    profile_count = 0; terminal_net_startup();

    since(); command("net forget Office");
    CHECK(seen("Forgot \"Office\".\n> ") && profile_count == 1);
    CHECK(!strcmp(file_text("/ram/.netprofiles"), "vantage-networks 1\n0 43616665 -\n"));
    since(); command("net forget Nope"); CHECK(seen("No saved network named \"Nope\".\n"));
    profile_count = NET_PROFILE_LIMIT;
    since(); join_network("net join --open Extra", NULL);
    CHECK(seen("Connected. Address 10.0.0.5. Not saved: 8 networks are already saved.\n"));
    profile_count = 0; profile_result = NET_UNSUPPORTED;
    since(); join_network("net join --open Plain", NULL); CHECK(seen("Address 10.0.0.5. Not saved.\n"));
    since(); command("net saved"); CHECK(seen("Not supported.\n"));
    since(); command("net forget Cafe"); CHECK(seen("Forgot \"Cafe\".") && !strstr(file_text("/ram/.netprofiles"), "4361"));
    profile_result = 0;
    since(); command("net saved"); CHECK(seen("No saved networks.\n"));
    file_capacity = 10;
    since(); join_network("net join --open Tiny", NULL);
    CHECK(seen("Saved for this session only (Storage or file capacity exhausted).\n"));
    file_capacity = sizeof(files[0].data);

    /* Status. */
    net_state.ssid_length = 4; memcpy(net_state.ssid, "Home", 4);
    net_state.security = NET_SECURITY_WPA2; net_state.rssi = -48; net_state.netmask = 0xffffff00U;
    net_state.gateway = 0x0a000001U; net_state.dns = 0x0a000001U; net_state.lease_seconds = 600;
    net_state.flags = NET_STATUS_HIDDEN | NET_STATUS_PROFILE; net_state.interface = NET_INTERFACE_WIFI;
    net_clock.synced = 1; net_clock.unix_seconds = 1794390067U;
    since(); command("net");
    CHECK(seen("Connected to Wi-Fi \"Home\" (WPA2, strong signal, hidden, saved)\nAddress: 10.0.0.5/24\n"
               "Router: 10.0.0.1  DNS: 10.0.0.1\nTime: 2026-11-11 09:41:07 UTC\n> "));
    net_clock.unix_seconds = 951868799U; net_state.flags = 0; net_state.rssi = -80; net_state.interface = 0;
    since(); command("net");   /* Kernels without Ethernet leave interface unset. */
    CHECK(seen("Connected to Wi-Fi \"Home\" (WPA2, weak signal)\n") && seen("Time: 2000-02-29 23:59:59 UTC\n"));
    net_state.interface = NET_INTERFACE_ETHERNET; net_state.ethernet = NET_ETHERNET_UP; net_state.ethernet_mbps = 1000;
    net_state.rssi = -60;
    since(); command("net");
    CHECK(seen("Connected by Ethernet (1000 Mbit/s)\nWi-Fi: connected to \"Home\" (WPA2, good signal)\nAddress: "));
    net_state.link = NET_LINK_RETRYING;
    since(); command("net"); CHECK(seen("Wi-Fi: reconnecting to \"Home\"\n"));
    net_state.interface = NET_INTERFACE_NONE; net_state.link = NET_LINK_DOWN; net_state.error = NET_AUTH;
    net_state.configuration = NET_ADDRESS_CONFIGURING; net_clock.synced = 0;
    since(); command("net");
    CHECK(seen("Not connected.\nWi-Fi: not connected\nAddress: requesting...\nTime: not synced\n"
               "Last error: Rejected by the network; check the passphrase.\n> "));
    CHECK(!strstr(transcript, "Frames") && !strstr(transcript, "ease"));
    net_state.error = 0; net_state.configuration = NET_ADDRESS_NONE;

    /* Passphrase prompt outcomes, join failures and Cancel. */
    unsigned int before = joins;
    command("net join Short"); text("1234567"); key(KEY_ENTER, 0);
    CHECK(joins == before && seen("Passphrase must be 8 to 63 characters.\n> ") && state_clear());
    since(); command("net join Cancelled"); text("secret-words"); key(KEY_CANCEL, 0);
    CHECK(joins == before && !t->secret && seen("Cancelled.\n> ") && !strstr(transcript, "secret-words") && state_clear());
    since(); command("net join Long");
    for (i = 0; i < 300; i++) key(KEY_TEXT, 'p');
    key(KEY_ENTER, 0);
    CHECK(joins == before && !t->secret && seen("Input too long; cancelled.\n> ") && state_clear());
    for (i = 0; i < TERMINAL_COMMAND_SIZE; i++) CHECK(!t->command[i]);
    join_result = NET_UNSUPPORTED;
    since(); command("net join --hidden --open Ghost"); CHECK(seen("Not supported.\n> ") && joined_flags == NET_JOIN_HIDDEN);
    join_result = NET_UNAVAILABLE;
    since(); command("net join --open X"); CHECK(seen("Wi-Fi unavailable (no radio firmware).\n> "));
    join_result = 0;
    net_state.link = NET_LINK_JOINING;
    since(); command("net join --open \"Cafe Wi-Fi\"");
    CHECK(!strcmp(joined_ssid, "Cafe Wi-Fi") && t->job.type);
    net_state.link = NET_LINK_DOWN; net_state.error = NET_AUTH;
    run(); CHECK(seen("Join failed: Rejected by the network; check the passphrase.\n"));
    net_state.link = NET_LINK_JOINING; net_state.error = 0;
    since(); command("net join --open Slow");
    unsigned int left = leaves;
    key(KEY_CANCEL, 0);
    CHECK(!t->job.type && leaves == left + 1 && seen("^C\nJoin cancelled.\n> "));
    command("net join --open Background");
    clock_now += 46000000U;
    since(); terminal_job_step(t);
    CHECK(!t->job.type && leaves == left + 1 && seen("Still joining in the background; see 'net'.\n"));
    join_result = NET_BUSY;
    since(); command("net join --open Busy"); CHECK(!t->job.type && seen("Busy; try again."));
    join_result = 0;
    since(); command("net leave");
    CHECK(leaves == left + 2 && seen("Disconnected. Saved networks wait until the next 'net join'.\n"));

    /* Scan. */
    scan_entries[0].rssi = -42; scan_entries[0].channel = 6; scan_entries[0].security = NET_SECURITY_WPA2;
    scan_entries[0].ssid_length = 10; memcpy(scan_entries[0].ssid, "VantageNet", 10);
    scan_entries[1].rssi = -75; scan_entries[1].security = NET_SECURITY_OPEN;
    scan_entries[1].ssid_length = 3; memcpy(scan_entries[1].ssid, "a\x01" "b", 3);
    scan_count = 2; scan_pending = 2;
    since(); command("net scan"); CHECK(t->job.type && seen("Scanning... (Cancel to stop)\n"));
    run();
    CHECK(seen("  Signal  Security    Network\n  strong  WPA2        \"VantageNet\"\n"
               "  weak    open        \"a?b\"\n> "));
    scan_count = 0;
    since(); command("net scan"); run(); CHECK(seen("No networks found.\n"));
    scan_pending = 1000;
    since(); command("net scan"); key(KEY_CANCEL, 0); CHECK(!t->job.type && seen("^C\nCancelled.\n> "));
    since(); command("net scan"); clock_now += 21000000U; run(); CHECK(seen("Timed out.\n"));
    scan_pending = 0;
    net_state.available = 0;
    since(); command("net scan"); CHECK(seen("Wi-Fi unavailable (no radio firmware).\n"));
    net_state.available = 1;

    /* Ping. */
    echo_replies = 1;
    since(); command("net ping vantage.test 2");
    for (i = 0; i < 4 && t->job.type; i++) { terminal_job_step(t); clock_now += 1000000U; }
    CHECK(!t->job.type && seen("Pinging vantage.test (192.168.50.10)...\nReply 1: ") && seen("Reply 2: "));
    CHECK(seen("2/2 replies, time 0-0 ms (average 0 ms).\n> "));
    echo_replies = 0;
    unsigned int closed = closes;
    since(); command("net ping vantage.test");
    terminal_job_step(t); clock_now += 1000000U; terminal_job_step(t);
    key(KEY_CANCEL, 0);
    CHECK(!t->job.type && seen("^C\n0/2 replies.\n> ") && closes == closed + 1);
    resolve_result = NET_AGAIN;
    since(); command("net ping vantage.test"); key(KEY_CANCEL, 0); CHECK(seen("^C\nCancelled.\n> "));
    resolve_result = 1;

    /* HTTP: headers hidden, Host with a non-default port. */
    serve("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nhello\x01world\r\n");
    connect_again = 2; receive_error = 0;
    since(); command("net get http://vantage.test:8080/path?q=1");
    run();
    CHECK(seen("Connecting to vantage.test...\nHTTP/1.0 200 OK\nhello?world\n\n> ") && !seen("Content-Type"));
    CHECK(strstr(request_text, "GET /path?q=1 HTTP/1.0\r\nHost: vantage.test:8080\r\n") == request_text);
    CHECK(strstr(request_text, "Connection: close\r\n\r\n") && state_clear());
    serve("HTTP/1.1 301 Moved\r\nX-Other: 1\r\nLOCATION: http://other/\r\n\r\nmoved");
    since(); command("net get http://vantage.test"); run();
    CHECK(seen("HTTP/1.1 301 Moved\nLocation: http://other/\nmoved\n") && strstr(request_text, "GET / HTTP/1.0\r\nHost: vantage.test\r\n"));
    serve("HTTP/1.0 200 OK\r\nLocation: hidden\r\n\r\n");
    since(); command("net get http://vantage.test/"); run(); CHECK(!seen("hidden"));
    serve("");
    since(); command("net get http://vantage.test/"); run(); CHECK(seen("(empty response)\n> "));
    {
        static char big[40100];
        memcpy(big, "HTTP/1.0 200 OK\r\n\r\n", 19);
        memset(big + 19, 'b', sizeof(big) - 20); big[sizeof(big) - 1] = 0;
        serve(big);
        unsigned int aborted = aborts;
        since(); command("net get http://vantage.test/big"); run();
        CHECK(seen("\n[Stopped after 32 KiB]\n> ") && aborts == aborted + 1);
        const char* body = strstr(transcript + mark, "bbb");
        CHECK(body && strspn(body, "b") == 32768);
    }
    serve("HTTP/1.0 200 OK\r\n\r\npartial"); receive_error = NET_RESET;
    since(); command("net get http://vantage.test/"); run(); CHECK(seen("partial\nConnection reset.\n> "));
    receive_error = 0;
    serve("HTTP/1.0 200 OK\r\n\r\nbody"); response_paused = 1;
    since(); command("net get http://vantage.test/"); terminal_job_step(t); terminal_job_step(t);
    clock_now += 31000000U; run(); CHECK(seen("Timed out.\n"));
    since(); command("net get http://vantage.test/"); terminal_job_step(t);
    key(KEY_CANCEL, 0); CHECK(!t->job.type && seen("^C\nCancelled.\n> ") && state_clear());
    response_paused = 0;

    /* Saving with -o: only 2xx bodies, relative to the current directory. */
    memset(files, 0, sizeof(files));
    static char page[700];
    memcpy(page, "HTTP/1.0 200 OK\r\nServer: x\r\n\r\n", 30);
    for (i = 30; i < 630; i++) page[i] = (char)('a' + i % 26);
    page[100] = '\r'; page[101] = '\n';
    serve(page);
    since(); command("net get -o page.txt http://vantage.test/page"); run();
    CHECK(seen("HTTP/1.0 200 OK\nSaved 600 bytes to /ram/page.txt.\n> ") && !seen("abc"));
    CHECK(files[0].size == 600 && !memcmp(files[0].data, page + 30, 600));
    serve("HTTP/1.0 404 Not Found\r\n\r\nmissing");
    since(); command("net get http://vantage.test/x -o other"); run();
    CHECK(seen("HTTP/1.0 404 Not Found\nNothing saved.\n> ") && !file_text("/ram/other") && !seen("missing"));
    serve("HTTP/1.0 302 Found\r\nLocation: /new\r\n\r\n");
    since(); command("net get http://vantage.test/x -o other"); run();
    CHECK(seen("Location: /new\nNothing saved.\n") && !file_text("/ram/other"));
    serve("HTTP/1.0 200 OK\r\nServer");
    since(); command("net get http://vantage.test/x -o other"); run();
    CHECK(seen("Incomplete response.\n> ") && !file_text("/ram/other"));
    serve(page); file_capacity = 300;
    unsigned int aborted = aborts;
    since(); command("net get http://vantage.test/x -o other"); run();
    CHECK(seen("Could not save: Storage or file capacity exhausted.\nNothing saved.\n> ") && !file_text("/ram/other"));
    CHECK(aborts == aborted + 1);
    file_capacity = sizeof(files[0].data);
    serve(page); response_length = 330; receive_error = NET_AGAIN;
    since(); command("net get http://vantage.test/x -o other");
    terminal_job_step(t); terminal_job_step(t); CHECK(t->job.type && file_text("/ram/other"));
    key(KEY_CANCEL, 0); receive_error = 0;
    CHECK(seen("Cancelled.\nNothing saved.\n> ") && !file_text("/ram/other"));
    storage_mounted = 0;
    serve(page);
    since(); command("net get http://vantage.test/x -o other"); run();
    CHECK(seen("Could not save: Storage unavailable (not mounted).\n> "));

    /* HTTPS through the TLS session, with partial sends and error mapping. */
    serve("HTTP/1.0 200 OK\r\n\r\nsecret page");
    tls_handshakes = 2; closed = closes;
    since(); command("net get https://secure.test/a"); run();
    CHECK(seen("HTTP/1.0 200 OK\nsecret page\n> ") && !strcmp(tls_host, "secure.test") && tls_closes == 1);
    CHECK(strstr(request_text, "GET /a HTTP/1.0\r\nHost: secure.test\r\n") && strstr(request_text, "\r\n\r\n"));
    CHECK(closes == closed + 1);
    since(); command("net get https://secure.test:8443/"); run();
    CHECK(strstr(request_text, "Host: secure.test:8443\r\n"));
    tls_open_result = TLS_NO_TRUST;
    since(); command("net get https://secure.test/"); run();
    CHECK(seen("Secure connection failed: no trusted certificates.\n> ") && tls_closes == 2);
    tls_open_result = 0; tls_handshake_result = TLS_CERTIFICATE;
    since(); command("net get https://secure.test/"); run();
    CHECK(seen("Secure connection failed: certificate rejected.\n> ") && tls_closes == 3);
    tls_handshake_result = 1; tls_receive_error = NET_RESET;
    serve("HTTP/1.0 200 OK\r\n\r\nx");
    since(); command("net get https://secure.test/"); run(); CHECK(seen("x\nConnection reset.\n> "));
    guards();
}

int main(void) {
    Terminal* t = &guarded.terminal;
    uint32_t i;
    CHECK(!terminal_init(NULL, 60, 50, NULL, NULL, NULL));
    CHECK(!terminal_init(t, 0, 50, NULL, NULL, NULL));
    CHECK(!terminal_init(t, 60, 0, NULL, NULL, NULL));
    CHECK(!terminal_init(t, TEXT_MAX_COLUMNS + 1, 50, NULL, NULL, NULL));
    CHECK(!terminal_init(t, 60, TERMINAL_MAX_ROWS + 1, NULL, NULL, NULL));
    reset(60, 50);
    CHECK(t->row == 3 && t->column == 2 && strstr(transcript, "Vantage terminal"));
    text("helx"); key(KEY_BACKSPACE, 0); text("p"); key(KEY_ENTER, 0);
    CHECK(strstr(transcript, "help   Show commands\nclear  Clear the terminal\n"));
    CHECK(strstr(transcript, "net    Network and Wi-Fi ('net help')\nabout  About this shell\n> "));
    command("mem");
    CHECK(strstr(transcript, "RAM: 128 MiB") && strstr(transcript, "Free pages: 123/456"));
    CHECK(strstr(transcript, "18446744073709551615/8388608"));
    info_kind = CONSOLE_MEMORY_HOST;
    command("mem");
    CHECK(strstr(transcript, "Simulator: host-managed memory.") && strstr(transcript, "1536000 bytes"));
    command("  echo hello terminal  ");
    CHECK(strstr(transcript, "\nhello terminal  \n"));
    command("echo"); CHECK(strstr(transcript, "> echo\n\n> "));
    command("unknown"); CHECK(strstr(transcript, "Unknown command"));
    command("helpme"); CHECK(strstr(transcript, "> helpme\nUnknown command"));
    command("about"); CHECK(strstr(transcript, "built-in shell"));
    command("edit notes"); CHECK(!t->editing && strstr(transcript, "Storage unavailable"));
    command("pwd"); CHECK(strstr(transcript, "> pwd\n/ram\n"));
    command("cd"); command("ls"); CHECK(strstr(transcript, "> ls\nStorage unavailable"));
    command("clear");
    CHECK(!t->row && t->column == 2 && t->cells[0][0] == '>');
    for (i = 2; i < t->columns; i++) CHECK(t->cells[0][i] == ' ');
    key(KEY_BACKSPACE, 0); CHECK(t->column == 2); /* Cannot erase the prompt. */
    text("echo keep"); key(KEY_CLEAR, 0);
    CHECK(!strcmp(t->command, "echo keep") && !t->row && t->column == 11);
    key(KEY_CANCEL, 0); CHECK(!t->length && strstr(transcript, "^C\n> "));
    key(KEY_TEXT, 0x100); key(KEY_TEXT, 0); CHECK(!t->length);

    reset(16, 4);
    text("echo 123456789012345");
    for (i = 0; i < 15; i++) key(KEY_BACKSPACE, 0);
    CHECK(!strcmp(t->command, "echo "));
    text("OK"); key(KEY_ENTER, 0); CHECK(strstr(transcript, "\nOK\n"));
    text("clear");
    for (i = 0; i < 200; i++) key(KEY_TEXT, ' ');
    key(KEY_ENTER, 0);
    CHECK(strstr(transcript, "command discarded"));
    guards();
    for (i = 0; i < 200; i++) command("echo scroll");
    CHECK(t->row == t->rows - 1);
    guards();

    /* Deterministic mixed input exercises wraps, scrolling and bounded editing. */
    { uint32_t seed = 17;
      for (i = 0; i < 20000; i++) {
          seed = seed * 1664525U + 1013904223U;
          if (seed % 13 == 0) key(KEY_ENTER, 0);
          else if (seed % 17 == 0) key(KEY_CLEAR, 0);
          else if (seed % 7 == 0) key(KEY_BACKSPACE, 0);
          else if (seed % 19 == 0) key(KEY_CANCEL, 0);
          else key(KEY_TEXT, 'a' + seed % 26);
          guards();
      }
    }
    /* Exercise editor geometry and bounds at both minimum and normal sizes. */
    for (uint32_t size = 0; size < 2; size++) {
        reset(size ? 60 : 40, size ? 33 : 12);
        t->editing = 1;
        t->edit_position = t->edit_top = t->edit_left = t->edit_modified = t->edit_confirm = 0;
        t->edit_status = "Test"; t->edit_touch = ~0U;
        uint32_t seed = 71;
        for (i = 0; i < 4000; i++) {
            seed = seed * 1664525U + 1013904223U;
            if (seed % 5 == 0) terminal_editor_action(t, EDIT_UP + seed % 6);
            else if (seed % 7 == 0) key(KEY_BACKSPACE, 0);
            else if (seed % 11 == 0) key(KEY_ENTER, 0);
            else key(KEY_TEXT, 'a' + seed % 26);
            TouchEvent touch = {0};
            touch.type = TOUCH_EVENT_DOWN; touch.x = -1; touch.y = -1;
            terminal_touch(t, &touch); touch.type = TOUCH_EVENT_UP; terminal_touch(t, &touch);
            CHECK(t->edit_position <= t->document_size && t->document_size <= TERMINAL_DOCUMENT_CAPACITY);
            guards();
        }
    }
    network_tests();
    puts("PASS: net commands, option parsing, masked and scrubbed passphrases, saved networks file, get -o and HTTPS");
    puts("PASS: table-driven shell commands, memory source labels, editing, wrapping, scrolling and input bounds");
    return 0;
}
