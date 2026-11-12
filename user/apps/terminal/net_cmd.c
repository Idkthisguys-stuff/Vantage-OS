#include "net.h"

#define SCAN_TIMEOUT_US 20000000U
#define JOIN_TIMEOUT_US 45000000U
#define RESOLVE_TIMEOUT_US 15000000U

uint32_t net_elapsed(uint32_t since) { return sys_time_us() - since; }

int net_equals(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void net_address(Terminal* t, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        terminal_number(t, (value >> shift) & 255U);
        if (shift) terminal_put(t, '.');
    }
}

void net_ssid(Terminal* t, const char* ssid, uint32_t length) {
    terminal_put(t, '"');
    for (uint32_t i = 0; i < length; i++) terminal_put(t, ssid[i] >= 32 && ssid[i] <= 126 ? ssid[i] : '?');
    terminal_put(t, '"');
}

void net_error(Terminal* t, int error) {
    const char* message = "Network error";
    switch (error) {
    case NET_BAD: message = "Invalid request"; break;
    case NET_UNAVAILABLE: message = "Network unavailable"; break;
    case NET_OFFLINE: message = "Not connected; use 'net join'"; break;
    case NET_TIMEOUT: message = "Timed out"; break;
    case NET_NO_SPACE: message = "Too many open connections"; break;
    case NET_REFUSED: message = "Connection refused"; break;
    case NET_RESET: message = "Connection reset"; break;
    case NET_NOT_FOUND: message = "Host not found"; break;
    case NET_AUTH: message = "Rejected by the network; check the passphrase"; break;
    case NET_BUSY: message = "Busy; try again"; break;
    case NET_UNREACHABLE: message = "Host unreachable"; break;
    case NET_UNSUPPORTED: message = "Not supported"; break;
    default: break;
    }
    terminal_write(t, message);
}

void net_fail(Terminal* t, int error) {
    net_error(t, error);
    terminal_write(t, ".\n");
}

static void wifi_fail(Terminal* t, int error) {
    if (error == NET_UNAVAILABLE) terminal_write(t, "Wi-Fi unavailable (no radio firmware).\n");
    else net_fail(t, error);
}

const char* net_argument(const char* text, char* out, uint32_t capacity, uint32_t* length) {
    while (*text == ' ') text++;
    *length = 0;
    int quoted = *text == '"';
    if (quoted) text++;
    while (*text && (quoted ? *text != '"' : *text != ' ')) {
        if (*length + 1 >= capacity) return NULL;
        out[(*length)++] = *text++;
    }
    if (quoted && (*text++ != '"' || (*text && *text != ' '))) return NULL;
    out[*length] = 0;
    return text;
}

int net_rest_empty(const char* text) {
    while (*text == ' ') text++;
    return !*text;
}

int net_resolve_step(Terminal* t, const char* host, uint32_t* address) {
    int result = sys_net_resolve(host, address);
    if (result == NET_AGAIN && net_elapsed(t->job.started) < RESOLVE_TIMEOUT_US) return 0;
    if (result > 0) return 1;
    terminal_write(t, host); terminal_write(t, ": ");
    net_fail(t, result == NET_AGAIN ? NET_TIMEOUT : result);
    terminal_job_end(t);
    return -1;
}

static void usage(Terminal* t, const char* syntax) {
    terminal_write(t, "Usage: "); terminal_write(t, syntax); terminal_write(t, "\n");
}

static const char* security_name(uint32_t security) {
    static const char* names[] = {"open", "WEP", "WPA", "WPA2", "WPA3", "WPA2/3", "enterprise", "other"};
    return security < sizeof(names) / sizeof(names[0]) ? names[security] : "other";
}

static const char* signal_name(int32_t rssi) {
    return rssi >= -55 ? "strong" : rssi >= -70 ? "good" : "weak";
}

static void two_digits(Terminal* t, uint32_t value, char after) {
    terminal_put(t, (char)('0' + value / 10 % 10));
    terminal_put(t, (char)('0' + value % 10));
    terminal_put(t, after);
}

/* Days since 1970-01-01 to a proleptic Gregorian date (Hinnant's algorithm). */
static void clock_text(Terminal* t, uint64_t seconds) {
    uint64_t z = seconds / 86400U + 719468U, era = z / 146097U, doe = z - era * 146097U;
    uint64_t yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
    uint64_t doy = doe - (365U * yoe + yoe / 4U - yoe / 100U), mp = (5U * doy + 2U) / 153U;
    uint32_t day = (uint32_t)(doy - (153U * mp + 2U) / 5U + 1U), month = (uint32_t)(mp < 10 ? mp + 3 : mp - 9);
    uint32_t second = (uint32_t)(seconds % 86400U);
    terminal_number(t, yoe + era * 400U + (month <= 2)); terminal_put(t, '-');
    two_digits(t, month, '-'); two_digits(t, day, ' ');
    two_digits(t, second / 3600U, ':'); two_digits(t, second / 60U % 60U, ':');
    two_digits(t, second % 60U, ' ');
    terminal_write(t, "UTC\n");
}

static void wifi_details(Terminal* t, const NetStatus* s) {
    net_ssid(t, s->ssid, s->ssid_length);
    terminal_write(t, " ("); terminal_write(t, security_name(s->security));
    if (s->rssi) { terminal_write(t, ", "); terminal_write(t, signal_name(s->rssi)); terminal_write(t, " signal"); }
    if (s->flags & NET_STATUS_HIDDEN) terminal_write(t, ", hidden");
    if (s->flags & NET_STATUS_PROFILE) terminal_write(t, ", saved");
    terminal_write(t, ")\n");
}

static void status(Terminal* t, const char* args) {
    (void)args;
    NetStatus s;
    NetTime time = {0};
    int result = sys_net_status(&s);
    if (result < 0) { net_fail(t, result); return; }
    /* Kernels without Ethernet support leave interface unset. */
    uint32_t active = s.interface ? s.interface : s.link == NET_LINK_UP ? NET_INTERFACE_WIFI : NET_INTERFACE_NONE;
    if (active == NET_INTERFACE_ETHERNET) {
        terminal_write(t, "Connected by Ethernet");
        if (s.ethernet_mbps) { terminal_write(t, " ("); terminal_number(t, s.ethernet_mbps); terminal_write(t, " Mbit/s)"); }
        terminal_put(t, '\n');
    } else if (active == NET_INTERFACE_WIFI) {
        terminal_write(t, "Connected to Wi-Fi "); wifi_details(t, &s);
    } else terminal_write(t, "Not connected.\n");
    if (active != NET_INTERFACE_WIFI) {
        terminal_write(t, "Wi-Fi: ");
        if (!s.available) terminal_write(t, s.error ? "stopped after a radio error\n" : "unavailable (no radio firmware)\n");
        else if (s.link == NET_LINK_UP) { terminal_write(t, "connected to "); wifi_details(t, &s); }
        else if (s.link == NET_LINK_JOINING) { terminal_write(t, "joining "); net_ssid(t, s.ssid, s.ssid_length); terminal_put(t, '\n'); }
        else if (s.link == NET_LINK_RETRYING) { terminal_write(t, "reconnecting to "); net_ssid(t, s.ssid, s.ssid_length); terminal_put(t, '\n'); }
        else terminal_write(t, "not connected\n");
    }
    if (s.configuration == NET_ADDRESS_BOUND) {
        uint32_t prefix = 0;
        for (uint32_t mask = s.netmask; mask & 0x80000000U; mask <<= 1) prefix++;
        terminal_write(t, "Address: "); net_address(t, s.address); terminal_put(t, '/'); terminal_number(t, prefix);
        terminal_write(t, "\nRouter: ");
        if (s.gateway) net_address(t, s.gateway); else terminal_write(t, "none");
        terminal_write(t, "  DNS: ");
        if (s.dns) net_address(t, s.dns); else terminal_write(t, "none");
        terminal_put(t, '\n');
    } else if (s.configuration == NET_ADDRESS_CONFIGURING) terminal_write(t, "Address: requesting...\n");
    if (sys_net_time(&time) > 0 && time.synced) { terminal_write(t, "Time: "); clock_text(t, time.unix_seconds); }
    else terminal_write(t, "Time: not synced\n");
    if (s.error && s.available && s.link != NET_LINK_UP) {
        terminal_write(t, "Last error: "); net_fail(t, s.error);
    }
}

static void scan_step(Terminal* t, void* state) {
    (void)state;
    NetScan entry = {0};
    entry.operation = NET_SCAN_RESULT;
    int result = sys_net_scan(&entry);
    if (result == NET_AGAIN) {
        if (net_elapsed(t->job.started) >= SCAN_TIMEOUT_US) { net_fail(t, NET_TIMEOUT); terminal_job_end(t); }
        return;
    }
    if (result < 0) { wifi_fail(t, result); terminal_job_end(t); return; }
    uint32_t found = 0;
    for (; result == 1; result = sys_net_scan(&entry)) {
        if (!found++) terminal_write(t, "  Signal  Security    Network\n");
        const char* columns[] = {signal_name(entry.rssi), security_name(entry.security)};
        for (uint32_t c = 0; c < 2; c++) {
            uint32_t width = 0;
            for (terminal_write(t, "  "); columns[c][width]; width++) terminal_put(t, columns[c][width]);
            for (; width < (c ? 10U : 6U); width++) terminal_put(t, ' ');
        }
        terminal_write(t, "  "); net_ssid(t, entry.ssid, entry.ssid_length); terminal_put(t, '\n');
    }
    if (!found) terminal_write(t, "No networks found.\n");
    terminal_job_end(t);
}
static const TerminalJobType scan_job = {scan_step, NULL};

typedef struct {
    uint32_t flags, ssid_length;
    char ssid[NET_SSID_MAX + 1];
    char passphrase[NET_PASSPHRASE_MAX + 1];
} JoinJob;
NET_STATE_FITS(JoinJob);

static void join_save(Terminal* t, JoinJob* j) {
    NetProfile profile = {0};
    uint32_t length = 0;
    while (j->passphrase[length]) length++;
    profile.operation = NET_PROFILE_ADD;
    profile.flags = j->flags & NET_JOIN_HIDDEN ? NET_PROFILE_HIDDEN : 0;
    profile.ssid_length = j->ssid_length;
    profile.passphrase_length = length;
    memcpy(profile.ssid, j->ssid, j->ssid_length);
    memcpy(profile.passphrase, j->passphrase, length);
    int result = sys_net_profile(&profile);
    if (result == NET_NO_SPACE) { terminal_write(t, " Not saved: 8 networks are already saved.\n"); return; }
    if (result < 0) { terminal_write(t, " Not saved.\n"); return; }
    result = net_profiles_store(j->ssid, j->ssid_length, j->passphrase, profile.flags);
    if (!result) terminal_write(t, " Saved; joins automatically at boot.\n");
    else if (result == FILE_UNAVAILABLE) terminal_write(t, " Saved for this session only (no storage).\n");
    else {
        terminal_write(t, " Saved for this session only ("); terminal_write(t, terminal_file_error_text(result));
        terminal_write(t, ").\n");
    }
}

static void join_step(Terminal* t, void* state) {
    JoinJob* j = state;
    NetStatus s;
    int result = sys_net_status(&s);
    if (result < 0) { net_fail(t, result); terminal_job_end(t); return; }
    if (s.link == NET_LINK_UP && s.configuration == NET_ADDRESS_BOUND) {
        terminal_write(t, "Connected. Address "); net_address(t, s.address); terminal_put(t, '.');
        join_save(t, j);
        terminal_job_end(t);
    } else if (s.link == NET_LINK_DOWN || s.link == NET_LINK_RETRYING) {
        terminal_write(t, "Join failed: "); net_fail(t, s.error ? s.error : NET_IO);
        if (s.link == NET_LINK_RETRYING) (void)sys_net_leave();
        terminal_job_end(t);
    } else if (net_elapsed(t->job.started) >= JOIN_TIMEOUT_US) {
        terminal_write(t, "Still joining in the background; see 'net'.\n");
        terminal_job_end(t);
    }
}

static void join_cancel(Terminal* t, void* state) {
    (void)state;
    (void)sys_net_leave();
    terminal_write(t, "Join cancelled.\n");
    terminal_job_end(t);
}
static const TerminalJobType join_job = {join_step, join_cancel};

static void join_start(Terminal* t, JoinJob* j) {
    int result = sys_net_join(j->ssid, j->passphrase, j->flags);
    if (result < 0) { wifi_fail(t, result); return; }
    terminal_write(t, "Joining "); net_ssid(t, j->ssid, j->ssid_length); terminal_write(t, "... (Cancel to stop)\n");
    terminal_job_run(t, &join_job);
}

static void join_passphrase(Terminal* t, void* state, const char* text) {
    JoinJob* j = state;
    uint32_t length = 0;
    while (text[length]) length++;
    if (length < NET_PASSPHRASE_MIN || length > NET_PASSPHRASE_MAX) {
        terminal_write(t, "Passphrase must be 8 to 63 characters.\n");
        return;
    }
    memcpy(j->passphrase, text, length + 1);
    join_start(t, j);
}

static void join(Terminal* t, const char* args) {
    JoinJob* j = terminal_job_state(t);
    uint32_t open = 0, length, named = 0;
    char word[NET_SSID_MAX + 2];
    while (!net_rest_empty(args)) {
        while (*args == ' ') args++;
        int option = args[0] == '-' && args[1] == '-';
        args = net_argument(args, word, sizeof(word), &length);
        if (!args) break;
        if (option && net_equals(word, "--open")) open = 1;
        else if (option && net_equals(word, "--hidden")) j->flags |= NET_JOIN_HIDDEN;
        else if (option || named || !length || length > NET_SSID_MAX) { args = NULL; break; }
        else { memcpy(j->ssid, word, length + 1); j->ssid_length = length; named = 1; }
    }
    if (!args || !named) { usage(t, "net join NAME [--open] [--hidden]"); return; }
    if (open) join_start(t, j);
    else terminal_ask_passphrase(t, join_passphrase);
}

static void leave(Terminal* t, const char* args) {
    (void)args;
    int result = sys_net_leave();
    if (result < 0) wifi_fail(t, result);
    else terminal_write(t, "Disconnected. Saved networks wait until the next 'net join'.\n");
}

static void saved(Terminal* t, const char* args) {
    NetProfile entry = {0};
    uint32_t count = 0;
    int result = 0;
    (void)args;
    entry.operation = NET_PROFILE_LIST;
    while (count <= NET_PROFILE_LIMIT && (result = sys_net_profile(&entry)) == 1) {
        if (!count++) terminal_write(t, "Saved networks:\n");
        terminal_write(t, "  "); net_ssid(t, entry.ssid, entry.ssid_length);
        if (entry.flags & NET_PROFILE_HIDDEN) terminal_write(t, " (hidden)");
        if (!entry.passphrase_length) terminal_write(t, " (open)");
        terminal_put(t, '\n');
        entry.operation = NET_PROFILE_LIST;
    }
    if (result < 0) net_fail(t, result);
    else if (!count) terminal_write(t, "No saved networks.\n");
}

static void forget(Terminal* t, const char* args) {
    char ssid[NET_SSID_MAX + 2];
    uint32_t length;
    args = net_argument(args, ssid, sizeof(ssid), &length);
    if (!args || !length || length > NET_SSID_MAX || !net_rest_empty(args)) { usage(t, "net forget NAME"); return; }
    NetProfile profile = {0};
    profile.operation = NET_PROFILE_REMOVE;
    profile.ssid_length = length;
    memcpy(profile.ssid, ssid, length);
    int kernel = sys_net_profile(&profile), stored = net_profiles_remove(ssid, length);
    if (!kernel || stored == 1) {
        terminal_write(t, "Forgot "); net_ssid(t, ssid, length); terminal_write(t, ".\n");
        if (stored < 0 && stored != FILE_UNAVAILABLE) {
            terminal_write(t, "Saved file not updated: "); terminal_write(t, terminal_file_error_text(stored));
            terminal_write(t, ".\n");
        }
    } else if (kernel == NET_NOT_FOUND || kernel == NET_UNSUPPORTED) {
        terminal_write(t, "No saved network named "); net_ssid(t, ssid, length); terminal_write(t, ".\n");
    } else net_fail(t, kernel);
}

static void scan(Terminal* t, const char* args) {
    NetScan request = {0};
    (void)args;
    request.operation = NET_SCAN_START;
    int result = sys_net_scan(&request);
    if (result < 0) { wifi_fail(t, result); return; }
    terminal_write(t, "Scanning... (Cancel to stop)\n");
    terminal_job_run(t, &scan_job);
}

static void help(Terminal* t, const char* args);

/* fixed: the subcommand takes no arguments. help NULL: not listed. */
static const struct {
    const char *name, *syntax, *help;
    void (*run)(Terminal* t, const char* args);
    uint32_t fixed;
} subcommands[] = {
    {"", "net", "Connection status", status, 1},
    {"scan", "net scan", "Nearby Wi-Fi networks", scan, 1},
    {"join", "net join NAME", "Join Wi-Fi (--open, --hidden)", join, 0},
    {"leave", "net leave", "Disconnect Wi-Fi", leave, 1},
    {"saved", "net saved", "Saved Wi-Fi networks", saved, 1},
    {"forget", "net forget NAME", "Forget a saved network", forget, 0},
    {"ping", "net ping HOST [N]", "Check that a host answers", net_ping, 0},
    {"lookup", "net lookup HOST", "Find a host's address", net_lookup, 0},
    {"get", "net get URL [-o FILE]", "Fetch a web page", net_get, 0},
    {"help", "net help", NULL, help, 1},
};

static void help(Terminal* t, const char* args) {
    (void)args;
    for (uint32_t i = 0; i < sizeof(subcommands) / sizeof(subcommands[0]); i++) {
        if (!subcommands[i].help) continue;
        uint32_t width = 0;
        for (; subcommands[i].syntax[width]; width++) terminal_put(t, subcommands[i].syntax[width]);
        for (; width < 22; width++) terminal_put(t, ' ');
        terminal_write(t, subcommands[i].help);
        terminal_put(t, '\n');
    }
}

void terminal_net_command(Terminal* t, const char* args) {
    char word[8];
    uint32_t length;
    const char* rest = net_argument(args, word, sizeof(word), &length);
    for (uint32_t i = 0; rest && i < sizeof(subcommands) / sizeof(subcommands[0]); i++) {
        if (!net_equals(word, subcommands[i].name)) continue;
        while (*rest == ' ') rest++;
        if (subcommands[i].fixed && *rest) usage(t, subcommands[i].syntax);
        else subcommands[i].run(t, rest);
        return;
    }
    terminal_write(t, "Unknown net command. Type 'net help'.\n");
}
