#include "net.h"

#define PING_INTERVAL_US 1000000U
#define PING_WAIT_US 2000000U
#define PING_BYTES 56U
#define PING_WINDOW 16U

typedef struct {
    uint32_t resolved, count, sent, received, address, timer;
    uint32_t rtt_min, rtt_max, rtt_total, sent_at[PING_WINDOW];
    uint64_t socket;
    char host[NET_NAME_MAX];
} PingJob;
NET_STATE_FITS(PingJob);

static const char* host_argument(const char* args, char host[NET_NAME_MAX]) {
    uint32_t length;
    args = net_argument(args, host, NET_NAME_MAX, &length);
    return args && length ? args : NULL;
}

static void lookup_step(Terminal* t, void* state) {
    PingJob* j = state;
    if (net_resolve_step(t, j->host, &j->address) <= 0) return;
    terminal_write(t, j->host); terminal_write(t, " is "); net_address(t, j->address); terminal_put(t, '\n');
    terminal_job_end(t);
}
static const TerminalJobType lookup_job = {lookup_step, NULL};

void net_lookup(Terminal* t, const char* args) {
    PingJob* j = terminal_job_state(t);
    args = host_argument(args, j->host);
    if (!args || !net_rest_empty(args)) { terminal_write(t, "Usage: net lookup HOST\n"); return; }
    terminal_job_run(t, &lookup_job);
}

static void ping_end(Terminal* t, PingJob* j) {
    if (j->sent) {
        terminal_number(t, j->received); terminal_put(t, '/'); terminal_number(t, j->sent);
        terminal_write(t, " replies");
        if (j->received) {
            terminal_write(t, ", time "); terminal_number(t, j->rtt_min / 1000U); terminal_put(t, '-');
            terminal_number(t, j->rtt_max / 1000U); terminal_write(t, " ms (average ");
            terminal_number(t, j->rtt_total / j->received / 1000U); terminal_write(t, " ms)");
        }
        terminal_write(t, ".\n");
    }
    if (j->socket) (void)sys_net_close(j->socket);
    terminal_job_end(t);
}

static void ping_step(Terminal* t, void* state) {
    PingJob* j = state;
    if (!j->resolved) {
        if (net_resolve_step(t, j->host, &j->address) <= 0) return;
        int64_t handle = sys_net_open(NET_ICMP);
        int result = handle < 0 ? (int)handle : sys_net_connect((uint64_t)handle, j->address, 0);
        if (handle >= 0) j->socket = (uint64_t)handle;
        if (result < 0) { net_fail(t, result); ping_end(t, j); return; }
        terminal_write(t, "Pinging "); terminal_write(t, j->host); terminal_write(t, " (");
        net_address(t, j->address); terminal_write(t, ")...\n");
        j->resolved = 1;
        j->timer = sys_time_us() - PING_INTERVAL_US;
    }
    if (j->sent < j->count && net_elapsed(j->timer) >= PING_INTERVAL_US) {
        NetSocketCall call = {0};
        for (uint32_t i = 0; i < PING_BYTES; i++) call.data[i] = (unsigned char)i;
        call.operation = NET_SOCKET_SEND;
        call.handle = j->socket;
        call.port = (uint16_t)(j->sent + 1U);
        call.length = PING_BYTES;
        j->timer = sys_time_us();
        j->sent_at[(j->sent + 1U) % PING_WINDOW] = j->timer;
        int64_t result = sys_net_socket(&call);
        j->sent++;
        if (result < 0) { terminal_write(t, "Request "); terminal_number(t, j->sent); terminal_write(t, ": "); net_fail(t, (int)result); }
    }
    for (unsigned int i = 0; i < 8; i++) {
        NetSocketCall call = {0};
        call.operation = NET_SOCKET_RECEIVE;
        call.handle = j->socket;
        call.length = PING_BYTES;
        int64_t result = sys_net_socket(&call);
        if (result == NET_AGAIN) break;
        if (result < 0) { net_fail(t, (int)result); break; }
        uint32_t sequence = call.port;
        if (!sequence || sequence > j->sent || j->sent - sequence >= PING_WINDOW) continue;
        uint32_t rtt = sys_time_us() - j->sent_at[sequence % PING_WINDOW];
        j->received++;
        j->rtt_total += rtt;
        if (j->received == 1 || rtt < j->rtt_min) j->rtt_min = rtt;
        if (rtt > j->rtt_max) j->rtt_max = rtt;
        terminal_write(t, "Reply "); terminal_number(t, sequence); terminal_write(t, ": ");
        terminal_number(t, rtt / 1000U); terminal_put(t, '.'); terminal_number(t, (rtt / 100U) % 10U);
        terminal_write(t, " ms\n");
    }
    if (j->sent == j->count && (j->received >= j->sent || net_elapsed(j->timer) >= PING_WAIT_US)) ping_end(t, j);
}

static void ping_cancel(Terminal* t, void* state) {
    PingJob* j = state;
    if (!j->sent) terminal_write(t, "Cancelled.\n");
    ping_end(t, j);
}
static const TerminalJobType ping_job = {ping_step, ping_cancel};

void net_ping(Terminal* t, const char* args) {
    PingJob* j = terminal_job_state(t);
    char number[8];
    uint32_t digits = 0;
    args = host_argument(args, j->host);
    const char* end = args ? net_argument(args, number, sizeof(number), &digits) : NULL;
    j->count = digits ? 0 : 4;
    for (uint32_t i = 0; i < digits; i++) {
        if (number[i] < '0' || number[i] > '9') { j->count = 0; break; }
        j->count = j->count * 10U + (uint32_t)(number[i] - '0');
    }
    if (!end || !net_rest_empty(end) || !j->count || j->count > 100) {
        terminal_write(t, "Usage: net ping HOST [COUNT 1-100]\n"); return;
    }
    terminal_job_run(t, &ping_job);
}
