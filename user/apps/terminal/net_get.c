#include "net.h"
#include <vantage/tls.h>

#define GET_IDLE_US 30000000U
#define GET_PRINT_LIMIT 32768U

enum { GET_RESOLVE, GET_CONNECT, GET_HANDSHAKE, GET_SEND, GET_RECEIVE };
/* Response parts: status line (shown), headers (hidden), body. */
enum { PART_STATUS, PART_HEADERS, PART_BODY };

typedef struct {
    uint32_t phase, secure, session, port, address, timer, sent;
    uint32_t part, line, status, skip, echo, any, printed, saved, buffered, file_open;
    uint64_t socket, file;
    char host[NET_NAME_MAX], path[TERMINAL_COMMAND_SIZE], target[FILE_PATH_MAX];
    unsigned char buffer[FILE_IO_MAX];
} GetJob;
NET_STATE_FITS(GetJob);

static void get_end(Terminal* t, GetJob* j, uint32_t abort) {
    if (j->session) tls_close();
    if (j->socket) {
        NetSocketCall call = {0};
        call.operation = NET_SOCKET_CLOSE;
        call.handle = j->socket;
        call.flags = abort ? NET_CLOSE_ABORT : 0;
        (void)sys_net_socket(&call);
    }
    if (j->file_open) {
        (void)sys_file_close(j->file);
        if (abort) { (void)sys_file_remove(j->target); terminal_write(t, "Nothing saved.\n"); }
    }
    terminal_job_end(t);
}

static void get_fail(Terminal* t, GetJob* j, int error) {
    if (j->part == PART_BODY && !j->target[0]) terminal_put(t, '\n');
    if (error <= TLS_NO_TRUST) {
        terminal_write(t, "Secure connection failed: "); terminal_write(t, tls_error_text(error)); terminal_write(t, ".\n");
    } else net_fail(t, error);
    get_end(t, j, 1);
}

static void get_cancel(Terminal* t, void* state) {
    terminal_write(t, "Cancelled.\n");
    get_end(t, state, 1);
}

static int flush(Terminal* t, GetJob* j) {
    int result = j->buffered ? sys_file_write(j->file, j->buffer, j->buffered) : 0;
    if (result >= 0 && (uint32_t)result != j->buffered) result = FILE_IO_ERROR;
    if (result < 0) {
        terminal_write(t, "Could not save: "); terminal_write(t, terminal_file_error_text(result)); terminal_write(t, ".\n");
        return 0;
    }
    j->saved += j->buffered;
    j->buffered = 0;
    return 1;
}

static void show(Terminal* t, unsigned char c) {
    terminal_put(t, c == '\n' || (c >= 32 && c <= 126) ? (char)c : c == '\t' ? ' ' : '?');
}

static int body_begin(Terminal* t, GetJob* j) {
    j->part = PART_BODY;
    if (!j->target[0]) return 1;
    if (j->status / 100 != 2) { terminal_write(t, "Nothing saved.\n"); return 0; }
    int64_t handle = sys_file_open(j->target, FILE_WRITE | FILE_CREATE | FILE_TRUNCATE);
    if (handle < 0) {
        terminal_write(t, "Could not save: "); terminal_write(t, terminal_file_error_text((int)handle)); terminal_write(t, ".\n");
        return 0;
    }
    j->file = (uint64_t)handle;
    j->file_open = 1;
    return 1;
}

/* Returns 0 to stop the transfer; the reason has been printed. */
static int response_byte(Terminal* t, GetJob* j, unsigned char c) {
    if (j->part == PART_BODY) {
        if (j->target[0]) {
            j->buffer[j->buffered++] = c;
            return j->buffered < FILE_IO_MAX || flush(t, j);
        }
        if (c == '\r') return 1;
        if (j->printed++ == GET_PRINT_LIMIT) { terminal_write(t, "\n[Stopped after 32 KiB]\n"); return 0; }
        show(t, c);
        return 1;
    }
    j->any = 1;
    if (c == '\r') return 1;
    if (j->part == PART_STATUS) {
        if (c == '\n') { terminal_put(t, '\n'); j->part = PART_HEADERS; j->line = 0; return 1; }
        if (j->line == 1 && c >= '0' && c <= '9' && j->status < 100) j->status = j->status * 10U + (uint32_t)(c - '0');
        if (c == ' ') j->line++;
        show(t, c);
        return 1;
    }
    if (c == '\n') {
        if (j->echo) terminal_put(t, '\n');
        if (!j->line) return body_begin(t, j);
        j->line = j->skip = j->echo = 0;
        return 1;
    }
    static const char location[] = "location:";
    if (j->line < 9) {
        if ((c >= 'A' && c <= 'Z' ? c + 32 : c) != location[j->line]) j->skip = 1;
        else if (j->line == 8 && !j->skip && j->status / 100 == 3) { terminal_write(t, "Location:"); j->echo = 1; }
    } else if (j->echo) show(t, c);
    j->line++;
    return 1;
}

static uint32_t request(const GetJob* j, char out[NET_IO_MAX]) {
    uint32_t n = 0;
    const char* parts[] = {"GET ", j->path, " HTTP/1.0\r\nHost: ", j->host};
    for (uint32_t p = 0; p < 4; p++) for (const char* s = parts[p]; *s; s++) out[n++] = *s;
    if (j->port != (j->secure ? 443U : 80U)) {
        char digits[6]; uint32_t d = 0, port = j->port;
        do { digits[d++] = (char)('0' + port % 10U); port /= 10U; } while (port);
        out[n++] = ':';
        while (d) out[n++] = digits[--d];
    }
    static const char tail[] = "\r\nUser-Agent: Vantage-OS\r\nAccept: */*\r\nConnection: close\r\n\r\n";
    for (const char* s = tail; *s; s++) out[n++] = *s;
    return n;
}

static void finished(Terminal* t, GetJob* j) {
    if (j->part != PART_BODY) {
        if (j->target[0]) { terminal_write(t, "Incomplete response.\n"); get_end(t, j, 1); return; }
        terminal_write(t, j->any ? "\n" : "(empty response)\n");
    } else if (j->target[0]) {
        if (!flush(t, j)) { get_end(t, j, 1); return; }
        terminal_write(t, "Saved "); terminal_number(t, j->saved); terminal_write(t, " bytes to ");
        terminal_write(t, j->target); terminal_write(t, ".\n");
    } else terminal_put(t, '\n');
    get_end(t, j, 0);
}

static void get_step(Terminal* t, void* state) {
    GetJob* j = state;
    int result = 0;
    if (j->phase == GET_RESOLVE) {
        if (net_resolve_step(t, j->host, &j->address) <= 0) return;
        int64_t handle = sys_net_open(NET_TCP);
        if (handle < 0) { get_fail(t, j, (int)handle); return; }
        j->socket = (uint64_t)handle;
        j->phase = GET_CONNECT;
        j->timer = sys_time_us();
        terminal_write(t, "Connecting to "); terminal_write(t, j->host); terminal_write(t, "...\n");
    }
    if (j->phase == GET_CONNECT) {
        result = sys_net_connect(j->socket, j->address, (uint16_t)j->port);
        if (result >= 0 && j->secure) {
            result = tls_open(j->socket, j->host);
            j->session = result >= 0;
        }
        if (result >= 0) { j->phase = j->secure ? GET_HANDSHAKE : GET_SEND; j->timer = sys_time_us(); }
    }
    if (j->phase == GET_HANDSHAKE) {
        result = tls_handshake();
        if (result > 0) { j->phase = GET_SEND; j->timer = sys_time_us(); }
    }
    if (j->phase == GET_SEND) {
        char text[NET_IO_MAX];
        uint32_t n = request(j, text);
        result = j->secure ? tls_send(text + j->sent, n - j->sent) : sys_net_send(j->socket, text + j->sent, n - j->sent);
        if (result > 0) { j->sent += (uint32_t)result; j->timer = sys_time_us(); }
        if (j->sent == n) { j->phase = GET_RECEIVE; result = 0; }
    }
    for (unsigned int i = 0; j->phase == GET_RECEIVE && i < 8; i++) {
        unsigned char data[512];
        result = j->secure ? tls_receive(data, sizeof(data)) : sys_net_receive(j->socket, data, sizeof(data));
        if (result == 0) { finished(t, j); return; }
        if (result < 0) break;
        j->timer = sys_time_us();
        for (int k = 0; k < result; k++)
            if (!response_byte(t, j, data[k])) { get_end(t, j, 1); return; }
    }
    if (result < 0 && result != NET_AGAIN) get_fail(t, j, result);
    else if (net_elapsed(j->timer) >= GET_IDLE_US) get_fail(t, j, NET_TIMEOUT);
}
static const TerminalJobType get_job = {get_step, get_cancel};

static int prefix(const char* text, const char* start) {
    while (*start && *text == *start) { text++; start++; }
    return !*start;
}

static int parse_url(GetJob* j, const char* url) {
    uint32_t length = 0;
    if (prefix(url, "https://")) { url += 8; j->secure = 1; }
    else if (prefix(url, "http://")) url += 7;
    else return 0;
    j->port = j->secure ? 443U : 80U;
    while (*url && *url != '/' && *url != ':') {
        if (length == NET_NAME_MAX - 1) return 0;
        j->host[length++] = *url++;
    }
    if (!length) return 0;
    if (*url == ':') {
        uint32_t digits = 0;
        for (url++, j->port = 0; *url >= '0' && *url <= '9' && digits < 6; digits++) j->port = j->port * 10U + (uint32_t)(*url++ - '0');
        if (!digits || !j->port || j->port > 65535 || (*url && *url != '/')) return 0;
    }
    length = 0;
    if (*url != '/') j->path[length++] = '/';
    for (; *url; url++) {
        if (*url <= ' ' || *url > '~' || length == TERMINAL_COMMAND_SIZE - 1) return 0;
        j->path[length++] = *url;
    }
    return 1;
}

void net_get(Terminal* t, const char* args) {
    GetJob* j = terminal_job_state(t);
    char word[TERMINAL_COMMAND_SIZE];
    uint32_t length, urls = 0, valid = 1;
    while (valid && !net_rest_empty(args)) {
        args = net_argument(args, word, sizeof(word), &length);
        if (!args) valid = 0;
        else if (net_equals(word, "-o")) {
            args = net_argument(args, word, sizeof(word), &length);
            if (!args || !length || j->target[0]) valid = 0;
            else if (!terminal_file_path(t, word, j->target)) {
                terminal_write(t, "Invalid file name.\n");
                return;
            }
        } else if (urls++ || !parse_url(j, word)) valid = 0;
    }
    if (!valid || !urls) { terminal_write(t, "Usage: net get http(s)://HOST[:PORT][/PATH] [-o FILE]\n"); return; }
    terminal_job_run(t, &get_job);
}
