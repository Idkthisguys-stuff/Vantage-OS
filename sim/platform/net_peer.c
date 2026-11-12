/* Virtual network for the simulator and host tests. Checksums, DHCP, DNS, NTP
 * and TCP are implemented independently of kernel/net so each side checks the
 * other. Everything is single-threaded and driven by the caller's clock. */
#include "net_peer.h"
#include "tls_peer.h"
#include <stdio.h>
#include <string.h>

#define FRAME 1514U
#define QUEUE 128U
#define CONNECTIONS 4U
#define STREAM 65536U
#define SERVER_MSS 1460U
#define SERVER_FLIGHT (8U * SERVER_MSS)
#define PORTS 2U
#define LEASES 4U
#define NTP_UNIX_OFFSET 2208988800ULL

static const unsigned char router_mac[6] = {0x02, 0x56, 0x4e, 0x00, 0x00, 0x01};
static const unsigned char web_mac[6] = {0x02, 0x56, 0x4e, 0x00, 0x00, 0x0a};
static const unsigned char broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/* Port 0 is the access point, port 1 the wired switch port. Each station MAC
 * leases its own address, 192.168.50.100 upwards in order of first request. */
enum { WIFI_PORT, WIRED_PORT };
typedef struct { int used, bound, port; unsigned char mac[6]; uint32_t address; } Lease;

typedef struct {
    int used, established, client_fin, fin_wanted, fin_sent, fin_acked, linger_only;
    uint32_t host, client, iss, snd_una, snd_nxt, rcv_nxt, window, mss, rto, base, length, request, line_bytes;
    char line[128];
    uint16_t port, client_port;
    unsigned int retries;
    uint64_t timer, linger, zero_until;
    unsigned char out[STREAM];
} Connection;

static NetPeerOptions options;
static NetPeerStats stats;
static struct { unsigned char data[FRAME]; unsigned int bytes; uint64_t due; } queue[PORTS][QUEUE];
static unsigned int queue_head[PORTS], queue_count[PORTS], up_count, down_count;
static unsigned char held[FRAME];
static unsigned int held_bytes, held_port;
static unsigned char stations[PORTS][6], frame_out[FRAME];
static int associated, cable, port_in;
static uint32_t client_in;
static Lease leases[LEASES];
static uint64_t clock_ms;
static uint16_t ip_id;
static Connection connections[CONNECTIONS];
static uint32_t next_iss = 0x10000000U;

static const char page[] =
    "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nServer: vantage-virtual-network\r\n\r\n"
    "Hello from the Vantage virtual network!\n"
    "This page was served over TCP by the simulator's web host.\n";
static const char missing[] =
    "HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\n\r\nNo such page.\n";

static uint16_t get16(const unsigned char* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put16(unsigned char* p, uint32_t v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static void put32(unsigned char* p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v); }
static int before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

static uint16_t checksum(const unsigned char* data, unsigned int bytes, uint32_t sum) {
    for (unsigned int i = 0; i + 1 < bytes; i += 2) sum += (uint32_t)get16(data + i);
    if (bytes & 1U) sum += (uint32_t)data[bytes - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
    return (uint16_t)~sum;
}
static uint32_t pseudo(uint32_t source, uint32_t destination, unsigned int protocol, unsigned int bytes) {
    return (source >> 16) + (source & 0xffffU) + (destination >> 16) + (destination & 0xffffU) + protocol + bytes;
}

NetPeerOptions* net_peer_options(void) { return &options; }
const NetPeerStats* net_peer_stats(void) { return &stats; }

void net_peer_reset(const NetPeerOptions* settings) {
    memset(&options, 0, sizeof(options));
    if (settings) options = *settings;
    memset(&stats, 0, sizeof(stats));
    memset(connections, 0, sizeof(connections));
    memset(queue_head, 0, sizeof(queue_head));
    memset(queue_count, 0, sizeof(queue_count));
    up_count = down_count = held_bytes = held_port = 0;
    associated = cable = port_in = 0;
    memset(stations, 0, sizeof(stations));
    memset(leases, 0, sizeof(leases));
}

static Lease* lease_at(uint32_t address) {
    for (unsigned int i = 0; i < LEASES; i++) if (leases[i].used && leases[i].address == address) return &leases[i];
    return NULL;
}

/* Unicast goes to the port holding the destination's lease; anything else
 * answers the station whose frame is being handled. */
static int port_to(uint32_t address) {
    const Lease* l = address == 0xffffffffU ? NULL : lease_at(address);
    return l ? l->port : port_in;
}

static const unsigned char* mac_to(uint32_t address) {
    const Lease* l = lease_at(address);
    return l ? l->mac : stations[port_in];
}

static void push(unsigned int port, const unsigned char* frame, unsigned int bytes, uint64_t now) {
    if (queue_count[port] == QUEUE) { stats.dropped++; return; }
    unsigned int slot = (queue_head[port] + queue_count[port]++) % QUEUE;
    memcpy(queue[port][slot].data, frame, bytes);
    queue[port][slot].bytes = bytes;
    queue[port][slot].due = now + options.latency_ms;
}

static void deliver(unsigned int port, const unsigned char* frame, unsigned int bytes, uint64_t now) {
    down_count++;
    if (options.drop_down && down_count % options.drop_down == 0) { stats.dropped++; return; }
    if (options.reorder_down && down_count % options.reorder_down == 0 && !held_bytes) {
        memcpy(held, frame, bytes);
        held_bytes = bytes;
        held_port = port;
        return;
    }
    push(port, frame, bytes, now);
    if (options.duplicate_down && down_count % options.duplicate_down == 0) push(port, frame, bytes, now);
    if (held_bytes) { push(held_port, held, held_bytes, now); held_bytes = 0; }
}

static unsigned int output(unsigned int port, unsigned char* frame, unsigned int capacity, uint64_t now) {
    clock_ms = now;
    if (!queue_count[port] || queue[port][queue_head[port]].due > now) return 0;
    unsigned int bytes = queue[port][queue_head[port]].bytes;
    if (bytes > capacity) bytes = capacity;
    memcpy(frame, queue[port][queue_head[port]].data, bytes);
    queue_head[port] = (queue_head[port] + 1) % QUEUE;
    queue_count[port]--;
    stats.down++;
    return bytes;
}

unsigned int net_peer_output(unsigned char* frame, unsigned int capacity, uint64_t now) {
    return output(WIFI_PORT, frame, capacity, now);
}

unsigned int net_peer_wired_output(unsigned char* frame, unsigned int capacity, uint64_t now) {
    return output(WIRED_PORT, frame, capacity, now);
}

static void ip_out(uint32_t source, uint32_t destination, unsigned int protocol,
                   const unsigned char* payload, unsigned int bytes, uint64_t now) {
    unsigned char* ip = frame_out + 14;
    memcpy(frame_out, destination == 0xffffffffU ? broadcast_mac : mac_to(destination), 6);
    memcpy(frame_out + 6, source == NET_PEER_WEB ? web_mac : router_mac, 6);
    put16(frame_out + 12, 0x0800);
    ip[0] = 0x45; ip[1] = 0; put16(ip + 2, 20 + bytes); put16(ip + 4, ip_id++); put16(ip + 6, 0);
    ip[8] = 64; ip[9] = (unsigned char)protocol; put16(ip + 10, 0);
    put32(ip + 12, source); put32(ip + 16, destination);
    put16(ip + 10, checksum(ip, 20, 0));
    memcpy(ip + 20, payload, bytes);
    deliver((unsigned int)port_to(destination), frame_out, 34 + bytes, now);
}

static void udp_out(uint32_t source, uint16_t source_port, uint32_t destination, uint16_t port,
                    const unsigned char* data, unsigned int bytes, uint64_t now) {
    unsigned char datagram[1480];
    put16(datagram, source_port); put16(datagram + 2, port); put16(datagram + 4, 8 + bytes); put16(datagram + 6, 0);
    memcpy(datagram + 8, data, bytes);
    uint16_t sum = checksum(datagram, 8 + bytes, pseudo(source, destination, 17, 8 + bytes));
    put16(datagram + 6, sum ? sum : 0xffffU);
    ip_out(source, destination, 17, datagram, 8 + bytes, now);
}

static void arp_out(unsigned int operation, const unsigned char* mac, uint32_t sender, uint32_t target, uint64_t now) {
    unsigned char frame[42];
    const unsigned char* station = mac_to(target);
    memcpy(frame, operation == 1 ? broadcast_mac : station, 6);
    memcpy(frame + 6, mac, 6);
    put16(frame + 12, 0x0806);
    put16(frame + 14, 1); put16(frame + 16, 0x0800); frame[18] = 6; frame[19] = 4; put16(frame + 20, operation);
    memcpy(frame + 22, mac, 6); put32(frame + 28, sender);
    if (operation == 2) memcpy(frame + 32, station, 6); else memset(frame + 32, 0, 6);
    put32(frame + 38, target);
    deliver((unsigned int)port_to(target), frame, sizeof(frame), now);
}

static Lease* lease_for(const unsigned char* mac) {
    Lease* free_lease = NULL;
    for (unsigned int i = 0; i < LEASES; i++) {
        if (leases[i].used && !memcmp(leases[i].mac, mac, 6)) return &leases[i];
        if (!leases[i].used && !free_lease) free_lease = &leases[i];
    }
    if (free_lease) {
        free_lease->used = 1;
        memcpy(free_lease->mac, mac, 6);
        free_lease->address = NET_PEER_CLIENT + (uint32_t)(free_lease - leases);
    }
    return free_lease;
}

static void dhcp_input(const unsigned char* m, unsigned int bytes, uint64_t now) {
    if (bytes < 240 || m[0] != 1 || get32(m + 236) != 0x63825363U || memcmp(m + 28, stations[port_in], 6)) { stats.invalid++; return; }
    Lease* lease = lease_for(m + 28);
    if (!lease) return;
    lease->port = port_in;
    uint32_t client = lease->address;
    unsigned int type = 0;
    uint32_t requested = 0, server = 0, ciaddr = get32(m + 12);
    for (unsigned int i = 240; i < bytes;) {
        unsigned int code = m[i++];
        if (code == 0) continue;
        if (code == 255 || i >= bytes) break;
        unsigned int length = m[i++];
        if (length > bytes - i) { stats.invalid++; return; }
        if (code == 53 && length == 1) type = m[i];
        if (code == 50 && length == 4) requested = get32(m + i);
        if (code == 54 && length == 4) server = get32(m + i);
        i += length;
    }
    unsigned int reply;
    if (type == 1) reply = 2;
    else if (type == 3) {
        if (server) reply = server == NET_PEER_ROUTER && requested == client ? 5 : server == NET_PEER_ROUTER ? 6 : 0;
        else if (ciaddr) reply = ciaddr == client && !options.nak_renewal ? 5 : 6;
        else reply = requested == client ? 5 : 6;
    } else if (type == 7) { stats.dhcp_releases++; lease->bound = 0; return; }
    else return;
    if (!reply || options.dhcp_silent) return;
    unsigned char r[300] = {0};
    r[0] = 2; r[1] = 1; r[2] = 6;
    memcpy(r + 4, m + 4, 4);
    put32(r + 12, ciaddr);
    if (reply != 6) put32(r + 16, client);
    memcpy(r + 28, lease->mac, 6);
    put32(r + 236, 0x63825363U);
    unsigned char* o = r + 240;
    *o++ = 53; *o++ = 1; *o++ = (unsigned char)reply;
    *o++ = 54; *o++ = 4; put32(o, NET_PEER_ROUTER); o += 4;
    if (reply != 6) {
        *o++ = 51; *o++ = 4; put32(o, options.lease_seconds ? options.lease_seconds : 3600U); o += 4;
        *o++ = 1; *o++ = 4; put32(o, 0xffffff00U); o += 4;
        *o++ = 3; *o++ = 4; put32(o, NET_PEER_ROUTER); o += 4;
        *o++ = 6; *o++ = 4; put32(o, NET_PEER_ROUTER); o += 4;
        if (!options.no_ntp_option) {
            *o++ = 42; *o++ = options.ntp_web ? 8 : 4; put32(o, NET_PEER_ROUTER); o += 4;
            if (options.ntp_web) { put32(o, NET_PEER_WEB); o += 4; }
        }
    }
    *o = 255;
    if (reply == 5) stats.dhcp_acks++;
    if (reply == 6) stats.dhcp_naks++;
    udp_out(NET_PEER_ROUTER, 67, ciaddr && reply != 6 ? ciaddr : 0xffffffffU, 68, r, sizeof(r), now);
    if (reply == 5 && !lease->bound) {
        lease->bound = 1;
        arp_out(1, router_mac, NET_PEER_ROUTER, client, now);  /* Checks the station answers ARP. */
    }
}

static unsigned int encode(unsigned char* out, const char* name) {
    unsigned int at = 0;
    while (*name) {
        unsigned int length = 0;
        while (name[length] && name[length] != '.') length++;
        out[at++] = (unsigned char)length;
        memcpy(out + at, name, length);
        at += length; name += length;
        if (*name == '.') name++;
    }
    out[at++] = 0;
    return at;
}

static unsigned int answer_a(unsigned char* p, unsigned int owner, uint32_t address, uint32_t ttl) {
    put16(p, 0xc000U | owner); put16(p + 2, 1); put16(p + 4, 1); put32(p + 6, ttl); put16(p + 10, 4);
    put32(p + 12, address);
    return 16;
}

static void dns_input(uint32_t server, uint32_t client, uint16_t source_port, const unsigned char* q, unsigned int bytes,
                      uint64_t now) {
    stats.dns_queries++;
    if (options.dns_silent || bytes < 17 || (get16(q + 2) & 0x8000U) || get16(q + 4) != 1) return;
    char name[256];
    unsigned int at = 12, length = 0;
    while (at < bytes && q[at]) {
        unsigned int label = q[at++];
        if (label > 63 || at + label > bytes || length + label + 1 >= sizeof(name)) { stats.invalid++; return; }
        if (length) name[length++] = '.';
        for (unsigned int i = 0; i < label; i++) {
            char c = (char)q[at + i];
            name[length++] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
        }
        at += label;
    }
    name[length] = 0;
    if (at + 5 > bytes || get16(q + at + 1) != 1 || get16(q + at + 3) != 1) { stats.invalid++; return; }
    unsigned int question_end = at + 5;
    if (!strcmp(name, "slow.vantage.test")) return;
    unsigned char r[512];
    memcpy(r, q, question_end);
    put16(r + 2, 0x8180U);
    put16(r + 6, 0); put16(r + 8, 0); put16(r + 10, 0);
    unsigned int n = question_end, answers = 0;
    if (!strcmp(name, "router.vantage.test") || !strcmp(name, "pool.ntp.org")) {
        n += answer_a(r + n, 12, NET_PEER_ROUTER, 300); answers = 1;
    }
    else if (!strcmp(name, "vantage.test")) { n += answer_a(r + n, 12, NET_PEER_WEB, 300); answers = 1; }
    else if (!strcmp(name, "remote.vantage.test")) { n += answer_a(r + n, 12, NET_PEER_REMOTE, 300); answers = 1; }
    else if (!strcmp(name, "ttl.vantage.test")) { n += answer_a(r + n, 12, NET_PEER_WEB, 1); answers = 1; }
    else if (!strcmp(name, "www.vantage.test")) {
        /* CNAME to vantage.test, then an A record whose owner points into the CNAME data. */
        put16(r + n, 0xc00cU); put16(r + n + 2, 5); put16(r + n + 4, 1); put32(r + n + 6, 300);
        unsigned int target = n + 12, size = encode(r + target, "vantage.test");
        put16(r + n + 10, size);
        n = target + size;
        n += answer_a(r + n, target, NET_PEER_WEB, 300);
        answers = 2;
    } else if (!strcmp(name, "loop.vantage.test")) {
        put16(r + n, 0xc000U | n); put16(r + n + 2, 1); put16(r + n + 4, 1); put32(r + n + 6, 300);
        put16(r + n + 10, 4); put32(r + n + 12, NET_PEER_WEB);
        n += 16; answers = 1;
    } else if (!strcmp(name, "spoof.vantage.test")) {
        unsigned char forged[512];
        memcpy(forged, r, n);
        put16(forged, get16(q) ^ 0x5555U);
        unsigned int m = n + answer_a(forged + n, 12, 0x0a000001U, 300);
        put16(forged + 6, 1);
        udp_out(server, 53, client, source_port, forged, m, now);
        n += answer_a(r + n, 12, NET_PEER_WEB, 300); answers = 1;
    } else put16(r + 2, 0x8183U);  /* NXDOMAIN */
    put16(r + 6, answers);
    udp_out(server, 53, client, source_port, r, n, now);
}

static uint64_t unix_ms(uint64_t now) {
    return (options.ntp_epoch ? options.ntp_epoch : NET_PEER_UNIX_EPOCH) * 1000ULL + now;
}

uint64_t net_peer_unix_ms(void) { return unix_ms(clock_ms); }

/* An SNTP server (RFC 4330 section 5) on the router and the web host;
 * options.ntp_fault spoils the router's replies. */
static void ntp_input(uint32_t server, uint32_t client, uint16_t source_port, const unsigned char* q, unsigned int bytes,
                      uint64_t now) {
    stats.ntp_queries++;
    unsigned int version = bytes ? (q[0] >> 3) & 7U : 0;
    if (bytes < 48 || (q[0] & 7U) != 3 || version < 3 || version > 4) { stats.invalid++; return; }
    unsigned int fault = server == NET_PEER_ROUTER ? options.ntp_fault : NET_PEER_NTP_GOOD;
    if (fault == NET_PEER_NTP_SILENT) return;
    uint64_t time = unix_ms(now);
    uint32_t seconds = (uint32_t)(time / 1000U + NTP_UNIX_OFFSET);
    uint32_t fraction = (uint32_t)(((time % 1000U) << 32) / 1000U);
    unsigned char r[48] = {0};
    r[0] = (unsigned char)((version << 3) | 4U); r[1] = 2; r[2] = q[2]; r[3] = 0xec;
    put32(r + 4, 0x00000200U); put32(r + 8, 0x00000400U); put32(r + 12, 0xcb00717bU);
    put32(r + 16, seconds - 64U);
    memcpy(r + 24, q + 40, 8);
    put32(r + 32, seconds); put32(r + 36, fraction);
    put32(r + 40, seconds); put32(r + 44, fraction);
    if (fault != NET_PEER_NTP_GOOD) {
        unsigned char bad[48];
        memcpy(bad, r, sizeof(bad));
        uint32_t from = server;
        uint16_t port = 123;
        switch (fault) {
        case NET_PEER_NTP_KOD: bad[0] |= 0xc0; bad[1] = 0; memcpy(bad + 12, "RATE", 4); break;
        case NET_PEER_NTP_MODE: bad[0] = (unsigned char)((bad[0] & ~7U) | 5U); break;
        case NET_PEER_NTP_VERSION: bad[0] = (unsigned char)((bad[0] & ~0x38U) | (2U << 3)); break;
        case NET_PEER_NTP_ORIGIN: bad[31] ^= 1; break;
        case NET_PEER_NTP_UNSYNCED: bad[0] |= 0xc0; break;
        case NET_PEER_NTP_STRATUM: bad[1] = 16; break;
        case NET_PEER_NTP_ZERO: memset(bad + 40, 0, 8); break;
        case NET_PEER_NTP_PORT: port = 1123; break;
        case NET_PEER_NTP_SOURCE: from = NET_PEER_WEB; break;
        default: break;
        }
        udp_out(from, port, client, source_port, bad, sizeof(bad), now);
        if (!options.ntp_genuine) return;
    }
    udp_out(server, 123, client, source_port, r, sizeof(r), now);
}

static uint32_t advertised(const Connection* c, uint64_t now) {
    if (c->port == 9 && now < c->zero_until) return 0;
    uint32_t window = options.server_window ? options.server_window : 65535U;
    uint32_t space = STREAM - (c->length - c->base);
    if (c->port == 7 && space < window) window = space;
    return window > 65535U ? 65535U : window;
}

static unsigned char stream_byte(const Connection* c, uint32_t offset) {
    if (c->port == 19) return (unsigned char)(offset * 7U + 3U);
    return c->out[offset - c->base];
}

static void segment(Connection* c, uint32_t sequence, unsigned int flags, uint32_t offset, unsigned int bytes, uint64_t now) {
    unsigned char s[24 + SERVER_MSS];
    unsigned int header = (flags & 2U) ? 24U : 20U;
    put16(s, c->port); put16(s + 2, c->client_port); put32(s + 4, sequence);
    put32(s + 8, (flags & 0x10U) ? c->rcv_nxt : 0);
    s[12] = (unsigned char)((header / 4U) << 4); s[13] = (unsigned char)flags;
    put16(s + 14, advertised(c, now)); put16(s + 16, 0); put16(s + 18, 0);
    if (flags & 2U) { s[20] = 2; s[21] = 4; put16(s + 22, SERVER_MSS); }
    for (unsigned int i = 0; i < bytes; i++) s[header + i] = stream_byte(c, offset + i);
    put16(s + 16, checksum(s, header + bytes, pseudo(c->host, c->client, 6, header + bytes)));
    ip_out(c->host, c->client, 6, s, header + bytes, now);
    stats.segments++;
}

static void reset(uint32_t host, uint16_t port, uint16_t client_port, uint32_t sequence, uint32_t ack, unsigned int flags, uint64_t now) {
    unsigned char s[20] = {0};
    put16(s, port); put16(s + 2, client_port); put32(s + 4, sequence); put32(s + 8, ack);
    s[12] = 0x50; s[13] = (unsigned char)flags;
    put16(s + 16, checksum(s, 20, pseudo(host, client_in, 6, 20)));
    ip_out(host, client_in, 6, s, 20, now);
}

static void server_output(Connection* c, uint64_t now) {
    if (!c->established || c->linger_only) return;
    while (!c->fin_sent) {
        uint32_t next = c->snd_nxt - (c->iss + 1U);
        if (next >= c->length) break;
        uint32_t flight = c->snd_nxt - c->snd_una, window = c->window < SERVER_FLIGHT ? c->window : SERVER_FLIGHT;
        if (flight >= window) break;
        uint32_t bytes = c->length - next;
        if (bytes > window - flight) bytes = window - flight;
        if (bytes > c->mss) bytes = c->mss;
        segment(c, c->snd_nxt, 0x18U, next, bytes, now);
        c->snd_nxt += bytes;
        if (!c->timer) c->timer = now + c->rto;
    }
    if (c->fin_wanted && !c->fin_sent && c->snd_nxt - (c->iss + 1U) == c->length) {
        segment(c, c->snd_nxt, 0x11U, 0, 0, now);
        c->snd_nxt++;
        c->fin_sent = 1;
        if (!c->timer) c->timer = now + c->rto;
    }
}

static void append(Connection* c, const void* data, unsigned int bytes) {
    memcpy(c->out + (c->length - c->base), data, bytes);
    c->length += bytes;
}

static void serve_http(Connection* c, const unsigned char* data, unsigned int bytes) {
    static const char end[] = "\r\n\r\n";
    for (unsigned int i = 0; i < bytes && c->request < 4; i++) {
        if (c->line_bytes < sizeof(c->line) - 1) c->line[c->line_bytes++] = (char)data[i];
        c->request = data[i] == (unsigned char)end[c->request] ? c->request + 1 : data[i] == '\r' ? 1 : 0;
        if (c->request < 4) continue;
        stats.http_requests++;
        c->line[c->line_bytes] = 0;
        c->length = c->base;
        if (!strncmp(c->line, "GET / ", 6)) append(c, page, (unsigned int)strlen(page));
        else if (!strncmp(c->line, "GET /large ", 11)) {
            static const char header[] = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n";
            append(c, header, sizeof(header) - 1);
            for (unsigned int line = 0, length = 0; length < 20000U; line++) {
                char text[64];
                int n = snprintf(text, sizeof(text), "line %05u of the large test document\n", line);
                append(c, text, (unsigned int)n);
                length += (unsigned int)n;
            }
        } else append(c, missing, (unsigned int)strlen(missing));
        c->fin_wanted = 1;
    }
}

/* HTTPS (port 443): tls_peer terminates TLS, and serve_http answers on a
 * plaintext twin of each connection. */
static Connection https[CONNECTIONS];

static void tls_sink(void* context, const unsigned char* data, unsigned int bytes) {
    Connection* c = context;
    if (bytes > STREAM - (c->length - c->base)) { stats.invalid++; return; }
    append(c, data, bytes);
}

static void serve_https(Connection* c, const unsigned char* data, unsigned int bytes) {
    static unsigned char text[4096];
    unsigned int slot = (unsigned int)(c - connections);
    Connection* plain = &https[slot];
    int n = tls_peer_input(slot, data, bytes, text, sizeof(text), tls_sink, c);
    if (n > 0 && !plain->fin_wanted) serve_http(plain, text, (unsigned int)n);
    if (plain->length != plain->base) {
        tls_peer_write(slot, plain->out, plain->length - plain->base, tls_sink, c);
        plain->base = plain->length;
    }
    if ((plain->fin_wanted || n < 0) && !c->fin_wanted) {
        tls_peer_finish(slot, tls_sink, c);
        c->fin_wanted = 1;
    }
}

static Connection* find(uint32_t host, uint16_t port, uint16_t client_port) {
    for (unsigned int i = 0; i < CONNECTIONS; i++) {
        Connection* c = &connections[i];
        if (c->used && c->host == host && c->client == client_in && c->port == port && c->client_port == client_port) return c;
    }
    return NULL;
}

static void tcp_input(uint32_t source, uint32_t host, const unsigned char* s, unsigned int bytes, uint64_t now) {
    if (bytes < 20 || checksum(s, bytes, pseudo(source, host, 6, bytes))) { stats.invalid++; return; }
    unsigned int header = (s[12] >> 4) * 4U, flags = s[13] & 0x3fU;
    if (header < 20 || header > bytes) { stats.invalid++; return; }
    uint16_t client_port = get16(s), port = get16(s + 2);
    uint32_t sequence = get32(s + 4), ack = get32(s + 8), window = get16(s + 14);
    const unsigned char* data = s + header;
    unsigned int length = bytes - header;
    Connection* c = find(host, port, client_port);
    if (options.server_silent) return;
    if (!c) {
        if (flags & 4U) return;
        if ((flags & 2U) && !(flags & 0x10U) && (port == 80 || port == 7 || port == 9 || port == 19 || port == 443)) {
            for (unsigned int i = 0; i < CONNECTIONS && !c; i++) if (!connections[i].used) c = &connections[i];
            if (!c) return;
            memset(c, 0, sizeof(*c) - sizeof(c->out));
            c->used = 1; c->host = host; c->client = client_in; c->port = port; c->client_port = client_port;
            c->iss = next_iss; next_iss += 0x01000000U;
            c->snd_una = c->iss; c->snd_nxt = c->iss + 1U; c->rcv_nxt = sequence + 1U;
            c->window = window; c->mss = 536; c->rto = 200;
            for (unsigned int i = 20; i + 1 < header;) {
                if (s[i] == 0) break;
                if (s[i] == 1) { i++; continue; }
                if (s[i] == 2 && s[i + 1] == 4 && i + 4 <= header) c->mss = get16(s + i + 2);
                if (s[i + 1] < 2) break;
                i += s[i + 1];
            }
            if (c->mss > SERVER_MSS) c->mss = SERVER_MSS;
            if (port == 9 && options.close_window_ms) c->zero_until = now + options.close_window_ms;
            if (port == 443) {
                memset(&https[c - connections], 0, sizeof(*c) - sizeof(c->out));
                tls_peer_accept((unsigned int)(c - connections));
            }
            segment(c, c->iss, 0x12U, 0, 0, now);
            c->timer = now + c->rto;
            return;
        }
        stats.tcp_resets++;
        if (flags & 0x10U) reset(host, port, client_port, ack, 0, 0x04U, now);
        else reset(host, port, client_port, 0, sequence + length + ((flags & 3U) ? 1U : 0U), 0x14U, now);
        return;
    }
    if (c->linger_only) {
        if (flags & 1U) segment(c, c->snd_nxt, 0x10U, 0, 0, now);
        return;
    }
    if (flags & 4U) { stats.tcp_resets++; memset(c, 0, sizeof(*c) - sizeof(c->out)); return; }
    if (flags & 2U) {
        if (!c->established) segment(c, c->iss, 0x12U, 0, 0, now);
        return;
    }
    if (!(flags & 0x10U)) return;
    if (before(c->snd_una, ack) && !before(c->snd_nxt, ack)) {
        if (!c->established && ack == c->iss + 1U) {
            c->established = 1;
            if (port == 19) { c->length = options.chargen_bytes ? options.chargen_bytes : 100000U; c->fin_wanted = 1; }
        }
        uint32_t acked = ack - (c->iss + 1U);
        if (c->fin_sent && ack == c->snd_nxt) { c->fin_acked = 1; acked--; }
        if (c->port != 19 && acked > c->base) {
            memmove(c->out, c->out + (acked - c->base), c->length - acked);
            c->base = acked;
        }
        c->snd_una = ack;
        c->retries = 0;
        c->rto = 200;
        c->timer = c->snd_una == c->snd_nxt ? 0 : now + c->rto;
    }
    c->window = window;
    int reply = 0;
    if (length) {
        reply = 1;
        uint32_t room = advertised(c, now);
        if (sequence == c->rcv_nxt && room) {
            unsigned int take = length < room ? length : room;
            if (port == 80 && !c->fin_wanted) serve_http(c, data, take);
            else if (port == 443) serve_https(c, data, take);
            else if (port == 7) { append(c, data, take); stats.echoed += take; }
            else if (port == 9) stats.discarded += take;
            c->rcv_nxt += take;
        } else if (sequence == c->rcv_nxt) stats.probes++;
    }
    if ((flags & 1U) && sequence + length == c->rcv_nxt && !c->client_fin) {
        c->rcv_nxt++;
        c->client_fin = 1;
        reply = 1;
        if (port == 7 || port == 9) c->fin_wanted = 1;
    }
    if (reply) segment(c, c->snd_nxt, 0x10U, 0, 0, now);
    server_output(c, now);
    if (c->client_fin && c->fin_acked) { c->linger_only = 1; c->linger = now + 2000U; c->timer = 0; }
}

void net_peer_poll(uint64_t now) {
    clock_ms = now;
    if (held_bytes) { push(held_port, held, held_bytes, now); held_bytes = 0; }
    for (unsigned int i = 0; i < CONNECTIONS; i++) {
        Connection* c = &connections[i];
        if (!c->used) continue;
        if (c->linger_only) { if (now >= c->linger) c->used = 0; continue; }
        if (c->port == 9 && c->zero_until && now >= c->zero_until) {
            c->zero_until = 0;
            segment(c, c->snd_nxt, 0x10U, 0, 0, now);  /* Window update. */
        }
        if (!c->timer || now < c->timer) continue;
        if (++c->retries > 12) { c->used = 0; continue; }
        stats.tcp_retransmits++;
        c->rto = c->rto < 3200U ? c->rto * 2U : 3200U;
        c->timer = now + c->rto;
        if (!c->established) { segment(c, c->iss, 0x12U, 0, 0, now); continue; }
        c->snd_nxt = c->snd_una;
        if (!c->fin_acked) c->fin_sent = 0;
        c->timer = 0;
        server_output(c, now);
        if (!c->timer) c->timer = now + c->rto;
    }
}

static void input(int port, const unsigned char* f, unsigned int bytes, uint64_t now) {
    clock_ms = now;
    if (bytes < 14) return;
    up_count++;
    if (options.drop_up && up_count % options.drop_up == 0) { stats.dropped++; return; }
    stats.up++;
    port_in = port;
    memcpy(stations[port], f + 6, 6);
    unsigned int type = get16(f + 12);
    const unsigned char* p = f + 14;
    bytes -= 14;
    if (type == 0x0806) {
        if (bytes < 28 || get16(p) != 1 || get16(p + 2) != 0x0800) { stats.invalid++; return; }
        uint32_t target = get32(p + 24);
        if (get16(p + 6) == 1 && (target == NET_PEER_ROUTER || target == NET_PEER_WEB))
            arp_out(2, target == NET_PEER_WEB ? web_mac : router_mac, target, get32(p + 14), now);
        else if (get16(p + 6) == 2 && lease_at(get32(p + 14)) && !memcmp(p + 18, router_mac, 6))
            stats.arp_replies++;
        return;
    }
    if (type != 0x0800) return;
    if (bytes < 20 || p[0] != 0x45 || checksum(p, 20, 0)) { stats.invalid++; return; }
    unsigned int total = get16(p + 2);
    if (total < 20 || total > bytes) { stats.invalid++; return; }
    uint32_t source = get32(p + 12), destination = get32(p + 16);
    client_in = source;
    const unsigned char* payload = p + 20;
    unsigned int length = total - 20;
    int to_web = !memcmp(f, web_mac, 6), to_router = !memcmp(f, router_mac, 6);
    int broadcast = destination == 0xffffffffU;
    /* Each address is only reachable through its correct next hop. */
    if (!broadcast && !((destination == NET_PEER_WEB && to_web) ||
                        ((destination == NET_PEER_ROUTER || destination == NET_PEER_REMOTE) && to_router))) {
        stats.invalid++;
        return;
    }
    if (p[9] == 17) {
        if (length < 8 || get16(payload + 4) > length ||
            (get16(payload + 6) && checksum(payload, get16(payload + 4),
                                            pseudo(source, destination, 17, get16(payload + 4))))) { stats.invalid++; return; }
        uint16_t sport = get16(payload), dport = get16(payload + 2);
        unsigned int size = get16(payload + 4) - 8U;
        if (dport == 67 && (broadcast || destination == NET_PEER_ROUTER)) dhcp_input(payload + 8, size, now);
        else if (dport == 53 && destination == NET_PEER_ROUTER) dns_input(destination, source, sport, payload + 8, size, now);
        else if (dport == 123 && (destination == NET_PEER_ROUTER || destination == NET_PEER_WEB))
            ntp_input(destination, source, sport, payload + 8, size, now);
        else if (!broadcast) {
            unsigned char error[8 + 28];
            memset(error, 0, 8);
            error[0] = 3; error[1] = 3;
            memcpy(error + 8, p, 20);
            memcpy(error + 28, payload, 8);
            put16(error + 2, checksum(error, sizeof(error), 0));
            ip_out(destination, source, 1, error, sizeof(error), now);
        }
    } else if (p[9] == 1 && !broadcast) {
        if (length < 8 || checksum(payload, length, 0)) { stats.invalid++; return; }
        if (payload[0] == 8) {
            unsigned char reply[1480];
            memcpy(reply, payload, length);
            reply[0] = 0; put16(reply + 2, 0);
            put16(reply + 2, checksum(reply, length, 0));
            stats.echo_replies++;
            ip_out(destination, source, 1, reply, length, now);
        }
    } else if (p[9] == 6 && !broadcast) {
        if (destination == NET_PEER_ROUTER) {
            if (length >= 20 && !(payload[13] & 4U)) {
                stats.tcp_resets++;
                if (payload[13] & 0x10U) reset(destination, get16(payload + 2), get16(payload), get32(payload + 8), 0, 0x04U, now);
                else reset(destination, get16(payload + 2), get16(payload), 0, get32(payload + 4) + 1U, 0x14U, now);
            }
        } else tcp_input(source, destination, payload, length, now);
    }
}

void net_peer_input(const unsigned char* f, unsigned int bytes, uint64_t now) {
    if (associated) input(WIFI_PORT, f, bytes, now);
}

void net_peer_wired_input(const unsigned char* f, unsigned int bytes, uint64_t now) {
    if (cable) input(WIRED_PORT, f, bytes, now);
}

/* Forgets frames, connections and ARP checks of the stations on a port. */
static void detach(unsigned int port) {
    queue_head[port] = queue_count[port] = 0;
    if (held_port == port) held_bytes = 0;
    for (unsigned int i = 0; i < LEASES; i++) {
        if (!leases[i].used || leases[i].port != (int)port) continue;
        leases[i].bound = 0;
        for (unsigned int k = 0; k < CONNECTIONS; k++)
            if (connections[k].client == leases[i].address) memset(&connections[k], 0, sizeof(connections[k]));
    }
}

static const struct {
    const char* ssid;
    const char* passphrase;
    int32_t rssi;
    uint32_t channel, security, flag;
    int hidden;
} networks[] = {
    {"VantageNet", NET_PEER_PASSPHRASE, -42, 6, NET_SECURITY_WPA2, NET_PEER_AP_MAIN, 0},
    {"Vantage Guest", NULL, -61, 11, NET_SECURITY_OPEN, NET_PEER_AP_GUEST, 0},
    {"Neighbour WPA3", NULL, -78, 36, NET_SECURITY_WPA3, NET_PEER_AP_WPA3, 0},
    {"Vantage Hidden", NET_PEER_HIDDEN_PASSPHRASE, -55, 1, NET_SECURITY_WPA2, NET_PEER_AP_HIDDEN, 1},
};

unsigned int net_peer_scan(NetScan* entries, unsigned int capacity) {
    unsigned int count = 0;
    for (unsigned int i = 0; i < sizeof(networks) / sizeof(networks[0]) && count < capacity; i++) {
        if (networks[i].hidden || (options.aps_off & networks[i].flag)) continue;
        memset(&entries[count], 0, sizeof(entries[count]));
        entries[count].rssi = networks[i].rssi;
        entries[count].channel = networks[i].channel;
        entries[count].security = networks[i].security;
        entries[count].ssid_length = (uint32_t)strlen(networks[i].ssid);
        memcpy(entries[count].ssid, networks[i].ssid, entries[count].ssid_length);
        count++;
    }
    return count;
}

int net_peer_associate(const unsigned char* ssid, unsigned int ssid_bytes, const unsigned char* passphrase,
                       unsigned int passphrase_bytes, int hidden, int32_t* rssi) {
    stats.joins++;
    for (unsigned int i = 0; i < sizeof(networks) / sizeof(networks[0]); i++) {
        if (strlen(networks[i].ssid) != ssid_bytes || memcmp(networks[i].ssid, ssid, ssid_bytes)) continue;
        /* A hidden network only answers probes that name it. */
        if ((options.aps_off & networks[i].flag) || (networks[i].hidden && !hidden)) break;
        if (networks[i].security == NET_SECURITY_OPEN ? passphrase_bytes != 0
            : networks[i].security != NET_SECURITY_WPA2 || passphrase_bytes != strlen(networks[i].passphrase) ||
              memcmp(passphrase, networks[i].passphrase, passphrase_bytes)) return NET_AUTH;
        detach(WIFI_PORT);
        associated = 1;
        *rssi = networks[i].rssi;
        return 1;
    }
    return NET_NOT_FOUND;
}

void net_peer_disassociate(void) {
    associated = 0;
    detach(WIFI_PORT);
}

void net_peer_set_cable(int up) {
    cable = up != 0;
    if (!cable) detach(WIRED_PORT);
}
