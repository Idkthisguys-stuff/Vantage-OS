/* SNTPv4 unicast client (RFC 4330, RFC 5905 section 8). Servers come from
 * DHCP option 42, otherwise pool.ntp.org through the resolver. The clock is
 * kept as UTC microseconds at a net_now() instant. */
#include "internal.h"

#define NTP_PORT 123U
#define NTP_BYTES 48U
#define NTP_unix_time_OFFSET 2208988800ULL   /* Seconds from 1900 to 1970. */
#define NTP_PIVOT 3944678400U           /* 2025-01-01 in era 0; earlier values are era 1. */
#define REPLY_MS 2000U
#define TRIES 3U
#define RESYNC_MS 3600000U
#define RETRY_FIRST_MS 15000U
#define RETRY_MAX_MS 900000U

enum { SNTP_IDLE, SNTP_RESOLVING, SNTP_WAITING, SNTP_SLEEPING };

static struct {
    int state;
    unsigned int index, tries;
    uint32_t server, backoff, stamp[2];
    uint16_t port;
    uint64_t sent, next;
} query;
static struct { int synced; uint32_t server; uint64_t unix_time_us, at; } sync;

int net_sntp_port_in_use(uint16_t port) { return query.port && query.port == port; }

void net_sntp_reset(void) {
    memset(&query, 0, sizeof(query));
    memset(&sync, 0, sizeof(sync));
}

static unsigned int servers(uint32_t list[2]) {
    unsigned int count = 0;
    for (unsigned int i = 0; i < 2; i++) if (net_interface.ntp[i]) list[count++] = net_interface.ntp[i];
    return count;
}

static void sleep_for(uint32_t delay) {
    query.state = SNTP_SLEEPING;
    query.port = 0;
    query.next = net_now() + delay;
}

static void failed(void) {
    uint32_t delay = query.backoff ? query.backoff : RETRY_FIRST_MS;
    sleep_for(delay);
    query.backoff = delay * 2U > RETRY_MAX_MS ? RETRY_MAX_MS : delay * 2U;
}

/* Each request carries a fresh random transmit timestamp; the reply must
 * echo it as its originate timestamp. */
static void transmit(void) {
    uint32_t list[2];
    unsigned int count = servers(list);
    if (count) query.server = list[query.index % count];
    unsigned char m[NTP_BYTES] = {0};
    m[0] = 0x23;  /* LI 0, version 4, mode 3 (client). */
    do { query.stamp[0] = net_random(); query.stamp[1] = net_random(); } while (!(query.stamp[0] | query.stamp[1]));
    net_put32(m + 40, query.stamp[0]);
    net_put32(m + 44, query.stamp[1]);
    query.state = SNTP_WAITING;
    query.sent = net_now();
    query.tries++;
    (void)net_udp_send(net_interface.address, query.server, query.port, NTP_PORT, m, sizeof(m));
}

static void resolve(void) {
    uint32_t address = 0;
    int result = net_dns_resolve("pool.ntp.org", &address);
    if (result == NET_AGAIN) return;
    if (result != 1 || !address) { failed(); return; }
    query.server = address;
    transmit();
}

static void begin(void) {
    uint32_t list[2];
    query.tries = 0;
    query.port = net_port_ephemeral();
    if (!query.port) { failed(); return; }
    if (servers(list)) transmit();
    else { query.state = SNTP_RESOLVING; resolve(); }
}

void net_sntp_poll(void) {
    if (!net_interface.bound) return;
    uint64_t now = net_now();
    if (query.state == SNTP_IDLE || (query.state == SNTP_SLEEPING && now >= query.next)) begin();
    else if (query.state == SNTP_RESOLVING) resolve();
    else if (query.state == SNTP_WAITING && now - query.sent >= REPLY_MS) {
        query.index++;
        if (query.tries >= TRIES) failed();
        else transmit();
    }
}

int net_sntp_input(uint32_t source, uint16_t source_port, uint16_t port,
                   const unsigned char* m, unsigned int bytes) {
    if (!net_sntp_port_in_use(port)) return 0;
    if (query.state != SNTP_WAITING || source != query.server || source_port != NTP_PORT || bytes < NTP_BYTES) return 1;
    unsigned int leap = m[0] >> 6, version = (m[0] >> 3) & 7U, mode = m[0] & 7U, stratum = m[1];
    if (mode != 4 || version < 3 || version > 4 ||
        net_get32(m + 24) != query.stamp[0] || net_get32(m + 28) != query.stamp[1]) return 1;
    if (!stratum) {
        /* Kiss-o'-death: stop asking this server for now. */
        query.index++;
        failed();
        return 1;
    }
    uint32_t seconds = net_get32(m + 40), fraction = net_get32(m + 44);
    if (leap == 3 || stratum > 15 || !(seconds | fraction)) return 1;
    uint64_t now = net_now();
    uint64_t unix_time = seconds >= NTP_PIVOT ? seconds - NTP_unix_time_OFFSET : seconds + (1ULL << 32) - NTP_unix_time_OFFSET;
    sync.unix_time_us = unix_time * 1000000ULL + (((uint64_t)fraction * 1000000ULL) >> 32) + (now - query.sent) * 500ULL;
    sync.at = now;
    sync.server = source;
    sync.synced = 1;
    query.backoff = 0;
    sleep_for(RESYNC_MS);
    return 1;
}

int net_time(NetTime* time) {
    memset(time, 0, sizeof(*time));
    if (!sync.synced) return 0;
    uint64_t elapsed = net_now() - sync.at, us = sync.unix_time_us + elapsed * 1000ULL;
    time->synced = 1;
    time->server = sync.server;
    time->unix_seconds = us / 1000000ULL;
    time->microseconds = (uint32_t)(us % 1000000ULL);
    time->age_seconds = elapsed / 1000U > 0xffffffffULL ? 0xffffffffU : (uint32_t)(elapsed / 1000U);
    return 1;
}
