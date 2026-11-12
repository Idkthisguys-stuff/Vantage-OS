/* The production network service, IPv4 stack, DHCP, DNS, TCP and socket
 * syscalls against the independent virtual network in sim/platform, with
 * deterministic loss, duplication, reordering and latency. */
#include "kernel/net/internal.h"
#include "sim/platform/net_peer.h"
#include "sim/platform/net_sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* phase = "start";
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s, line %d: %s\n", phase, __LINE__, #c); exit(1); } } while (0)
#define PHASE(f) (phase = #f, f())

static uint64_t now_us;
static uint64_t seed = 0x2545f4914f6cdd1dULL;
static union { uint64_t align; unsigned char bytes[8192]; } arena;

uint32_t time_us(void) { return (uint32_t)now_us; }
uint32_t net_port_random(void) {
    seed ^= seed >> 12; seed ^= seed << 25; seed ^= seed >> 27;
    return (uint32_t)((seed * 0x2545f4914f6cdd1dULL) >> 32);
}
int net_entropy(void* data, unsigned int bytes) {
    unsigned char* out = data;
    for (unsigned int i = 0; i < bytes; i++) out[i] = (unsigned char)net_port_random();
    return 0;
}
uint32_t user_buffer_valid(uintptr_t buffer, size_t size) {
    uintptr_t start = (uintptr_t)arena.bytes;
    return buffer >= start && size <= sizeof(arena.bytes) && buffer - start <= sizeof(arena.bytes) - size;
}

static void tick(unsigned int ms) {
    for (unsigned int i = 0; i < ms; i++) { now_us += 1000; net_poll(); }
}

static NetStatus status(void) {
    NetStatus s;
    CHECK(net_status(&s) == 0);
    return s;
}

#define WAIT(condition, ms) do { unsigned int waited_ = 0; \
    while (!(condition)) { CHECK(waited_++ < (ms)); tick(1); } } while (0)

static int64_t sock(uint64_t owner, uint32_t operation, uint64_t handle, uint32_t address, uint16_t port,
                    const void* data, uint32_t length, NetSocketCall* out) {
    NetSocketCall call;
    memset(&call, 0, sizeof(call));
    call.operation = operation; call.handle = handle; call.address = address; call.port = port;
    call.length = length;
    if (operation == NET_SOCKET_OPEN) call.type = (uint32_t)handle, call.handle = 0;
    if (data && operation == NET_SOCKET_SEND) memcpy(call.data, data, length);
    int64_t result = net_socket(owner, &call);
    if (out) *out = call;
    return result;
}

static int resolve(const char* name, uint32_t* address, unsigned int budget) {
    NetResolve r;
    memset(&r, 0, sizeof(r));
    memcpy(r.name, name, strlen(name) + 1);
    int result;
    unsigned int waited = 0;
    while ((result = net_resolve(&r)) == NET_AGAIN) { CHECK(waited++ < budget); tick(1); }
    *address = r.address;
    return result;
}

static void join(const char* ssid, const char* passphrase) {
    NetJoin j;
    memset(&j, 0, sizeof(j));
    j.ssid_length = (uint32_t)strlen(ssid);
    memcpy(j.ssid, ssid, j.ssid_length);
    if (passphrase) { j.passphrase_length = (uint32_t)strlen(passphrase); memcpy(j.passphrase, passphrase, j.passphrase_length); }
    CHECK(net_join(&j) == 0);
}

static void connect_bound(void) {
    join("VantageNet", NET_PEER_PASSPHRASE);
    WAIT(status().configuration == NET_ADDRESS_BOUND, 20000);
}

static uint64_t tcp_open(uint32_t address, uint16_t port) {
    int64_t handle = sock(1, NET_SOCKET_OPEN, NET_TCP, 0, 0, NULL, 0, NULL);
    CHECK(handle > 0);
    int64_t result = sock(1, NET_SOCKET_CONNECT, (uint64_t)handle, address, port, NULL, 0, NULL);
    CHECK(result == NET_AGAIN);
    WAIT((result = sock(1, NET_SOCKET_CONNECT, (uint64_t)handle, address, port, NULL, 0, NULL)) != NET_AGAIN, 70000);
    CHECK(result == 0);
    return (uint64_t)handle;
}

static unsigned int used_sockets(void) {
    unsigned int count = 0;
    for (unsigned int i = 0; i < NET_SOCKET_LIMIT; i++) count += net_sockets[i].state != SOCKET_FREE;
    return count;
}

static unsigned char pattern(uint32_t i) { return (unsigned char)((i * 2654435761U) >> 13); }

/* Sends `bytes` through echo and checks every returned byte, interleaving. */
static void echo_transfer(uint32_t host, uint32_t bytes) {
    uint64_t h = tcp_open(host, 7);
    uint32_t sent = 0, received = 0;
    unsigned char chunk[NET_IO_MAX];
    unsigned int idle = 0;
    while (received < bytes) {
        if (sent < bytes) {
            uint32_t n = bytes - sent < NET_IO_MAX ? bytes - sent : NET_IO_MAX;
            for (uint32_t i = 0; i < n; i++) chunk[i] = pattern(sent + i);
            int64_t accepted = sock(1, NET_SOCKET_SEND, h, 0, 0, chunk, n, NULL);
            CHECK(accepted > 0 || accepted == NET_AGAIN);
            if (accepted > 0) sent += (uint32_t)accepted;
        }
        NetSocketCall call;
        int64_t got = sock(1, NET_SOCKET_RECEIVE, h, 0, 0, NULL, NET_IO_MAX, &call);
        CHECK(got > 0 || got == NET_AGAIN);
        if (got > 0) {
            for (int64_t i = 0; i < got; i++) CHECK(call.data[i] == pattern(received + (uint32_t)i));
            received += (uint32_t)got;
            idle = 0;
        } else { tick(1); CHECK(++idle < 120000); }
    }
    CHECK(sock(1, NET_SOCKET_CLOSE, h, 0, 0, NULL, 0, NULL) == 0);
    WAIT(used_sockets() == 0, 200000);
}

static void unavailable_and_literals(void) {
    NetStatus s = status();
    CHECK(!s.available && !s.link && !s.interface && !s.ethernet);
    NetJoin j = {4, 0, "test", ""};
    CHECK(net_join(&j) == NET_UNAVAILABLE);
    NetScan scan = {0};
    CHECK(net_scan(&scan) == NET_UNAVAILABLE);
    NetProfile saved = {NET_PROFILE_LIST};
    CHECK(net_profile(&saved) == NET_UNAVAILABLE);
    NetTime* time_out = (NetTime*)arena.bytes;
    memset(time_out, 0xa5, sizeof(*time_out));
    CHECK(handle_net_call(SYS_NET_TIME, (uintptr_t)time_out, 1) == 0);
    for (unsigned int i = 0; i < sizeof(*time_out); i++) CHECK(!arena.bytes[i]);
    memset(&saved, 0, sizeof(saved));
    memcpy(arena.bytes, &saved, sizeof(saved));
    CHECK(handle_net_call(SYS_NET_PROFILE, (uintptr_t)arena.bytes, 1) == NET_UNAVAILABLE);
    uint32_t address = 0;
    CHECK(resolve("10.1.2.3", &address, 1) == 1 && address == 0x0a010203U);
    CHECK(resolve("vantage.test", &address, 1) == NET_UNAVAILABLE);
    NetResolve bad = {1, 0, "1.2.3.4"};
    CHECK(net_resolve(&bad) == NET_BAD);
    int64_t h = sock(1, NET_SOCKET_OPEN, NET_TCP, 0, 0, NULL, 0, NULL);
    CHECK(h > 0 && sock(1, NET_SOCKET_CONNECT, (uint64_t)h, 0x0a000001U, 80, NULL, 0, NULL) == NET_OFFLINE);
    CHECK(sock(1, NET_SOCKET_CLOSE, (uint64_t)h, 0, 0, NULL, 0, NULL) == 0 && !used_sockets());
}

static void scan_and_join_failures(void) {
    NetScan scan = {0};
    CHECK(net_scan(&scan) == 0);
    scan.operation = NET_SCAN_RESULT;
    CHECK(net_scan(&scan) == NET_AGAIN);
    tick(900);
    int32_t last = 0;
    for (unsigned int i = 0; i < 3; i++) {
        CHECK(net_scan(&scan) == 1 && scan.cursor == i + 1);
        CHECK(!i || scan.rssi <= last);
        last = scan.rssi;
        if (!i) CHECK(scan.ssid_length == 10 && !memcmp(scan.ssid, "VantageNet", 10) && scan.security == NET_SECURITY_WPA2);
    }
    CHECK(net_scan(&scan) == 0);

    NetJoin j = {0};
    CHECK(net_join(&j) == NET_BAD);
    j.ssid_length = 3; memcpy(j.ssid, "abc", 3); j.passphrase_length = 7;
    CHECK(net_join(&j) == NET_BAD);
    j.passphrase_length = 8; memcpy(j.passphrase, "abc\ndefg", 8);
    CHECK(net_join(&j) == NET_BAD);

    join("VantageNet", "wrong-passphrase");
    CHECK(status().link == NET_LINK_JOINING);
    WAIT(status().link == NET_LINK_DOWN, 2000);
    NetStatus s = status();
    CHECK(s.error == NET_AUTH && !s.ssid_length);
    join("Neighbour WPA3", "something-long");
    WAIT(status().link == NET_LINK_DOWN, 2000);
    CHECK(status().error == NET_AUTH);
    join("Absent network", NULL);
    WAIT(status().link == NET_LINK_DOWN, 2000);
    CHECK(status().error == NET_NOT_FOUND);
    join("Vantage Guest", "should-be-open");
    WAIT(status().link == NET_LINK_DOWN, 2000);
}

static void bound_configuration(void) {
    connect_bound();
    NetStatus s = status();
    CHECK(s.link == NET_LINK_UP && s.address == NET_PEER_CLIENT && s.netmask == 0xffffff00U);
    CHECK(s.gateway == NET_PEER_ROUTER && s.dns == NET_PEER_ROUTER && s.security == NET_SECURITY_WPA2);
    CHECK(s.rssi == -42 && s.lease_seconds > 3590 && s.lease_seconds <= 3600 && !s.error);
    CHECK(s.ssid_length == 10 && !memcmp(s.ssid, "VantageNet", 10) && !s.flags);
    CHECK(s.interface == NET_INTERFACE_WIFI && s.ethernet == NET_ETHERNET_NO_CABLE && !s.ethernet_mbps);
    tick(50);
    CHECK(net_peer_stats()->dhcp_acks == 1 && net_peer_stats()->arp_replies == 1);
    CHECK(!net_peer_stats()->invalid);
}

static void icmp(void) {
    int64_t h = sock(1, NET_SOCKET_OPEN, NET_ICMP, 0, 0, NULL, 0, NULL);
    CHECK(h > 0);
    CHECK(sock(1, NET_SOCKET_SEND, (uint64_t)h, 0, 1, "x", 1, NULL) == NET_BAD);  /* No peer yet. */
    const uint32_t targets[] = {NET_PEER_WEB, NET_PEER_REMOTE, NET_PEER_ROUTER};
    for (unsigned int t = 0; t < 3; t++) {
        CHECK(sock(1, NET_SOCKET_CONNECT, (uint64_t)h, targets[t], 0, NULL, 0, NULL) == 0);
        for (uint16_t seq = 1; seq <= 3; seq++) {
            unsigned char payload[56];
            for (unsigned int i = 0; i < sizeof(payload); i++) payload[i] = (unsigned char)(i ^ seq);
            CHECK(sock(1, NET_SOCKET_SEND, (uint64_t)h, 0, seq, payload, sizeof(payload), NULL) == 56);
            NetSocketCall reply;
            int64_t got;
            WAIT((got = sock(1, NET_SOCKET_RECEIVE, (uint64_t)h, 0, 0, NULL, 64, &reply)) != NET_AGAIN, 3000);
            CHECK(got == 56 && reply.port == seq && reply.address == targets[t] && !memcmp(reply.data, payload, 56));
        }
    }
    /* An on-link host that never answers ARP becomes unreachable. */
    CHECK(sock(1, NET_SOCKET_CONNECT, (uint64_t)h, 0xc0a8324dU, 0, NULL, 0, NULL) == 0);
    CHECK(sock(1, NET_SOCKET_SEND, (uint64_t)h, 0, 9, "ping", 4, NULL) == 4);
    tick(3500);
    CHECK(sock(1, NET_SOCKET_SEND, (uint64_t)h, 0, 10, "ping", 4, NULL) == NET_UNREACHABLE);
    CHECK(sock(1, NET_SOCKET_CLOSE, (uint64_t)h, 0, 0, NULL, 0, NULL) == 0);
    CHECK(net_peer_stats()->echo_replies == 9);
}

static void dns(void) {
    uint32_t address = 0;
    uint64_t queries = net_peer_stats()->dns_queries;
    CHECK(resolve("vantage.test", &address, 3000) == 1 && address == NET_PEER_WEB);
    CHECK(resolve("VANTAGE.test.", &address, 0) == 1 && address == NET_PEER_WEB);  /* Cached. */
    CHECK(net_peer_stats()->dns_queries == queries + 1);
    CHECK(resolve("www.vantage.test", &address, 3000) == 1 && address == NET_PEER_WEB);
    CHECK(resolve("remote.vantage.test", &address, 3000) == 1 && address == NET_PEER_REMOTE);
    CHECK(resolve("spoof.vantage.test", &address, 3000) == 1 && address == NET_PEER_WEB);
    CHECK(resolve("missing.vantage.test", &address, 3000) == NET_NOT_FOUND);
    CHECK(resolve("loop.vantage.test", &address, 3000) == NET_NOT_FOUND);
    CHECK(resolve("slow.vantage.test", &address, 15000) == NET_TIMEOUT);
    CHECK(resolve("bad..name", &address, 0) == NET_BAD);
    CHECK(resolve("a-very-long-label-that-exceeds-sixty-three-characters-xxxxxxxxxxx.test", &address, 0) == NET_BAD);
    CHECK(resolve("under_score.vantage.test", &address, 3000) == NET_NOT_FOUND);
    queries = net_peer_stats()->dns_queries;
    CHECK(resolve("ttl.vantage.test", &address, 3000) == 1);
    tick(6000);  /* TTL 1 s is held for the 5 s minimum, then requeried. */
    CHECK(resolve("ttl.vantage.test", &address, 3000) == 1 && net_peer_stats()->dns_queries == queries + 2);
}

static void udp(void) {
    int64_t h = sock(1, NET_SOCKET_OPEN, NET_UDP, 0, 0, NULL, 0, NULL);
    CHECK(h > 0);
    NetSocketCall info;
    CHECK(sock(1, NET_SOCKET_STATUS, (uint64_t)h, 0, 0, NULL, 0, &info) == 0 && info.local_port >= 49152);
    CHECK(sock(1, NET_SOCKET_SEND, (uint64_t)h, 0, 0, "x", 1, NULL) == NET_BAD);
    CHECK(sock(1, NET_SOCKET_CONNECT, (uint64_t)h, NET_PEER_WEB, 9999, NULL, 0, NULL) == 0);
    CHECK(sock(1, NET_SOCKET_SEND, (uint64_t)h, 0, 0, "hello", 5, NULL) == 5);
    int64_t got;
    WAIT((got = sock(1, NET_SOCKET_RECEIVE, (uint64_t)h, 0, 0, NULL, 16, NULL)) != NET_AGAIN, 2000);
    CHECK(got == NET_REFUSED);  /* ICMP port unreachable from the web host. */
    CHECK(sock(1, NET_SOCKET_RECEIVE, (uint64_t)h, 0, 0, NULL, 16, NULL) == NET_AGAIN);
    CHECK(sock(1, NET_SOCKET_CLOSE, (uint64_t)h, 0, 0, NULL, 0, NULL) == 0);
}

static void http(void) {
    uint64_t h = tcp_open(NET_PEER_WEB, 80);
    static const char request[] = "GET / HTTP/1.0\r\nHost: vantage.test\r\n\r\n";
    CHECK(sock(1, NET_SOCKET_SEND, h, 0, 0, request, sizeof(request) - 1, NULL) == (int64_t)sizeof(request) - 1);
    char body[2048];
    unsigned int length = 0;
    for (;;) {
        NetSocketCall call;
        int64_t got = sock(1, NET_SOCKET_RECEIVE, h, 0, 0, NULL, NET_IO_MAX, &call);
        if (got == NET_AGAIN) { tick(1); continue; }
        CHECK(got >= 0);
        if (!got) break;
        CHECK(length + (size_t)got < sizeof(body));
        memcpy(body + length, call.data, (size_t)got);
        length += (unsigned int)got;
    }
    body[length] = 0;
    CHECK(strstr(body, "HTTP/1.0 200 OK") == body && strstr(body, "Hello from the Vantage virtual network!"));
    NetSocketCall state;
    CHECK(sock(1, NET_SOCKET_STATUS, h, 0, 0, NULL, 0, &state) == NET_TCP_PEER_CLOSED);
    CHECK(sock(1, NET_SOCKET_SEND, h, 0, 0, "late", 4, NULL) == 4);  /* Half-closed peer still reads. */
    CHECK(sock(1, NET_SOCKET_CLOSE, h, 0, 0, NULL, 0, NULL) == 0);
    WAIT(used_sockets() == 0, 5000);
    CHECK(sock(1, NET_SOCKET_STATUS, h, 0, 0, NULL, 0, NULL) == NET_BAD_HANDLE);
    CHECK(net_peer_stats()->http_requests == 1);
}

static void refused(void) {
    const struct { uint32_t host; uint16_t port; } targets[] = {{NET_PEER_WEB, 81}, {NET_PEER_ROUTER, 80}};
    for (unsigned int i = 0; i < 2; i++) {
        int64_t h = sock(1, NET_SOCKET_OPEN, NET_TCP, 0, 0, NULL, 0, NULL);
        int64_t result = sock(1, NET_SOCKET_CONNECT, (uint64_t)h, targets[i].host, targets[i].port, NULL, 0, NULL);
        WAIT((result = sock(1, NET_SOCKET_CONNECT, (uint64_t)h, targets[i].host, targets[i].port, NULL, 0, NULL)) != NET_AGAIN, 3000);
        CHECK(result == NET_REFUSED);
        CHECK(sock(1, NET_SOCKET_CONNECT, (uint64_t)h, NET_PEER_WEB, 80, NULL, 0, NULL) == NET_BUSY);
        CHECK(sock(1, NET_SOCKET_SEND, (uint64_t)h, 0, 0, "x", 1, NULL) == NET_REFUSED);
        CHECK(sock(1, NET_SOCKET_CLOSE, (uint64_t)h, 0, 0, NULL, 0, NULL) == 0);
    }
}

static void lossy_transfers(void) {
    NetPeerOptions* o = net_peer_options();
    o->latency_ms = 20; o->drop_up = 7; o->drop_down = 11; o->duplicate_down = 5; o->reorder_down = 13;
    echo_transfer(NET_PEER_WEB, 200000);
    echo_transfer(NET_PEER_REMOTE, 30000);
    CHECK(net_peer_stats()->tcp_retransmits > 0 && net_peer_stats()->echoed >= 230000);
    /* Download: every byte of the chargen stream, in order. */
    o->chargen_bytes = 100000;
    uint64_t h = tcp_open(NET_PEER_WEB, 19);
    uint32_t received = 0;
    for (;;) {
        NetSocketCall call;
        int64_t got = sock(1, NET_SOCKET_RECEIVE, h, 0, 0, NULL, NET_IO_MAX, &call);
        if (got == NET_AGAIN) { tick(1); continue; }
        CHECK(got >= 0);
        if (!got) break;
        for (int64_t i = 0; i < got; i++) CHECK(call.data[i] == (unsigned char)((received + i) * 7U + 3U));
        received += (uint32_t)got;
    }
    CHECK(received == 100000);
    CHECK(sock(1, NET_SOCKET_CLOSE, h, 0, 0, NULL, 0, NULL) == 0);
    WAIT(used_sockets() == 0, 10000);
    memset(o, 0, sizeof(*o));
}

/* Latency above the 1 s minimum RTO forces a spurious timeout. The ACK for
 * data sent before the rewind must still be accepted when it arrives. */
static void spurious_timeout(void) {
    uint64_t h = tcp_open(NET_PEER_WEB, 9);
    NetSocket* s = &net_sockets[(h & 255) - 1];
    net_peer_options()->latency_ms = 1500;
    unsigned char chunk[NET_IO_MAX];
    memset(chunk, 0x42, sizeof(chunk));
    /* Lose the per-segment ACKs so only one cumulative ACK comes back. */
    net_peer_options()->drop_down = 1;
    for (unsigned int i = 0; i < 4; i++) {
        if (i == 3) net_peer_options()->drop_down = 0;
        CHECK(sock(1, NET_SOCKET_SEND, h, 0, 0, chunk, sizeof(chunk), NULL) == NET_IO_MAX);
    }
    uint32_t sent = s->u.tcp.snd_max - s->u.tcp.snd_una;
    CHECK(sent == 4 * NET_IO_MAX);
    tick(1100);
    CHECK(s->u.tcp.snd_nxt - s->u.tcp.snd_una < sent && s->u.tcp.tx_used == sent);  /* Rewound. */
    tick(500);
    CHECK(s->u.tcp.tx_used == 0 && s->u.tcp.snd_una == s->u.tcp.snd_max && s->u.tcp.snd_nxt == s->u.tcp.snd_max);
    net_peer_options()->latency_ms = 0;
    CHECK(sock(1, NET_SOCKET_CLOSE, h, 0, 0, NULL, 0, NULL) == 0);
    WAIT(used_sockets() == 0, 70000);
}

static void zero_window(void) {
    NetPeerOptions* o = net_peer_options();
    o->close_window_ms = 8000;
    uint64_t discarded = net_peer_stats()->discarded;
    uint64_t h = tcp_open(NET_PEER_WEB, 9);
    unsigned char chunk[NET_IO_MAX];
    memset(chunk, 0x5a, sizeof(chunk));
    uint32_t sent = 0;
    unsigned int waited = 0;
    while (sent < 20000) {
        uint32_t n = 20000 - sent < NET_IO_MAX ? 20000 - sent : NET_IO_MAX;
        int64_t accepted = sock(1, NET_SOCKET_SEND, h, 0, 0, chunk, n, NULL);
        CHECK(accepted > 0 || accepted == NET_AGAIN);
        if (accepted > 0) sent += (uint32_t)accepted;
        else { tick(1); CHECK(++waited < 30000); }
    }
    CHECK(sock(1, NET_SOCKET_CLOSE, h, 0, 0, NULL, 0, NULL) == 0);
    WAIT(net_peer_stats()->discarded - discarded == 20000 && used_sockets() == 0, 60000);
    CHECK(net_peer_stats()->probes > 0);
    memset(o, 0, sizeof(*o));
}

static void timeouts(void) {
    NetPeerOptions* o = net_peer_options();
    o->server_silent = 1;
    int64_t h = sock(1, NET_SOCKET_OPEN, NET_TCP, 0, 0, NULL, 0, NULL);
    int64_t result = sock(1, NET_SOCKET_CONNECT, (uint64_t)h, NET_PEER_WEB, 80, NULL, 0, NULL);
    unsigned int waited = 0;
    while ((result = sock(1, NET_SOCKET_CONNECT, (uint64_t)h, NET_PEER_WEB, 80, NULL, 0, NULL)) == NET_AGAIN) {
        tick(10); waited += 10;
    }
    CHECK(result == NET_TIMEOUT && waited > 60000 && waited < 66000);
    CHECK(sock(1, NET_SOCKET_CLOSE, (uint64_t)h, 0, 0, NULL, 0, NULL) == 0);
    /* An established connection whose peer vanishes is aborted after retries. */
    o->server_silent = 0;
    uint64_t live = tcp_open(NET_PEER_WEB, 9);
    o->server_silent = 1;
    CHECK(sock(1, NET_SOCKET_SEND, live, 0, 0, "abcdef", 6, NULL) == 6);
    waited = 0;
    while ((result = sock(1, NET_SOCKET_RECEIVE, live, 0, 0, NULL, 16, NULL)) == NET_AGAIN) {
        tick(100); CHECK((waited += 100) < 600000);
    }
    CHECK(result == NET_TIMEOUT);
    CHECK(sock(1, NET_SOCKET_CLOSE, live, 0, 0, NULL, 0, NULL) == 0 && !used_sockets());
    o->server_silent = 0;
}

static void ownership_and_limits(void) {
    int64_t handles[NET_SOCKET_LIMIT];
    for (unsigned int i = 0; i < NET_SOCKET_LIMIT; i++) {
        handles[i] = sock(1, NET_SOCKET_OPEN, i % 2 ? NET_UDP : NET_ICMP, 0, 0, NULL, 0, NULL);
        CHECK(handles[i] > 0);
    }
    CHECK(sock(1, NET_SOCKET_OPEN, NET_TCP, 0, 0, NULL, 0, NULL) == NET_NO_SPACE);
    CHECK(sock(1, NET_SOCKET_OPEN, 9, 0, 0, NULL, 0, NULL) == NET_BAD);
    CHECK(sock(2, NET_SOCKET_STATUS, (uint64_t)handles[1], 0, 0, NULL, 0, NULL) == NET_BAD_HANDLE);
    CHECK(sock(2, NET_SOCKET_CLOSE, (uint64_t)handles[1], 0, 0, NULL, 0, NULL) == NET_BAD_HANDLE);
    CHECK(sock(1, NET_SOCKET_CLOSE, (uint64_t)handles[1], 0, 0, NULL, 0, NULL) == 0);
    CHECK(sock(1, NET_SOCKET_CLOSE, (uint64_t)handles[1], 0, 0, NULL, 0, NULL) == NET_BAD_HANDLE);
    int64_t reused = sock(2, NET_SOCKET_OPEN, NET_UDP, 0, 0, NULL, 0, NULL);
    CHECK(reused > 0 && reused != handles[1] && (reused & 255) == (handles[1] & 255));
    CHECK(sock(1, NET_SOCKET_STATUS, (uint64_t)reused, 0, 0, NULL, 0, NULL) == NET_BAD_HANDLE);
    CHECK(sock(1, 99, (uint64_t)handles[0], 0, 0, NULL, 0, NULL) == NET_BAD);
    CHECK(sock(1, NET_SOCKET_SEND, (uint64_t)handles[0], 0, 0, NULL, NET_IO_MAX + 1, NULL) == NET_BAD);
    CHECK(sock(0, NET_SOCKET_OPEN, NET_UDP, 0, 0, NULL, 0, NULL) == NET_UNAVAILABLE);
    net_release(1);
    CHECK(used_sockets() == 1);
    net_release(2);
    CHECK(!used_sockets());
    /* An unread TCP connection is reset when its owner goes away. */
    uint64_t h = tcp_open(NET_PEER_WEB, 19);
    tick(200);
    uint64_t resets = net_peer_stats()->tcp_resets;
    net_release(1);
    tick(10);
    CHECK(!used_sockets() && net_peer_stats()->tcp_resets == resets + 1);
    (void)h;
}

static void syscall_boundary(void) {
    NetStatus* status_out = (NetStatus*)arena.bytes;
    CHECK(handle_net_call(SYS_NET_STATUS, (uintptr_t)status_out, 1) == 0 && status_out->available);
    CHECK(handle_net_call(SYS_NET_STATUS, (uintptr_t)(arena.bytes + 4), 1) == NET_BAD);
    CHECK(handle_net_call(SYS_NET_STATUS, (uintptr_t)(arena.bytes + sizeof(arena.bytes) - 8), 1) == NET_BAD);
    NetStatus kernel_status;
    CHECK(handle_net_call(SYS_NET_STATUS, (uintptr_t)&kernel_status, 1) == NET_BAD);
    CHECK(handle_net_call(SYS_NET_STATUS, (uintptr_t)status_out, 0) == NET_UNAVAILABLE);
    NetResolve* r = (NetResolve*)arena.bytes;
    memset(r, 'a', sizeof(*r));
    r->flags = 0;
    CHECK(handle_net_call(SYS_NET_RESOLVE, (uintptr_t)r, 1) == NET_BAD);  /* Unterminated. */
    memcpy(r->name, "192.168.50.10", sizeof("192.168.50.10"));
    CHECK(handle_net_call(SYS_NET_RESOLVE, (uintptr_t)r, 1) == 1 && r->address == NET_PEER_WEB);
    NetSocketCall* call = (NetSocketCall*)arena.bytes;
    memset(call, 0, sizeof(*call));
    call->operation = NET_SOCKET_OPEN; call->type = NET_UDP;
    int64_t h = handle_net_call(SYS_NET_SOCKET, (uintptr_t)call, 1);
    CHECK(h > 0);
    call->operation = NET_SOCKET_RECEIVE; call->handle = (uint64_t)h; call->length = 0;
    CHECK(handle_net_call(SYS_NET_SOCKET, (uintptr_t)call, 1) == NET_BAD);
    call->length = 16;
    CHECK(handle_net_call(SYS_NET_SOCKET, (uintptr_t)call, 1) == NET_AGAIN);
    CHECK(handle_net_call(SYS_NET_SOCKET, (uintptr_t)(arena.bytes + 4), 1) == NET_BAD);
    call->operation = NET_SOCKET_CLOSE;
    CHECK(handle_net_call(SYS_NET_SOCKET, (uintptr_t)call, 1) == 0);
    NetScan* scan = (NetScan*)arena.bytes;
    memset(scan, 0, sizeof(*scan));
    scan->operation = 7;
    CHECK(handle_net_call(SYS_NET_SCAN, (uintptr_t)scan, 1) == NET_BAD);
    CHECK(handle_net_call(99, (uintptr_t)scan, 1) == NET_BAD);
}

static void dhcp_lifecycle(void) {
    NetPeerOptions* o = net_peer_options();
    CHECK(net_leave() == 0);
    tick(20);
    CHECK(net_peer_stats()->dhcp_releases == 1);
    NetStatus s = status();
    CHECK(s.link == NET_LINK_DOWN && !s.address && !s.ssid_length && !s.error);
    net_peer_reset(NULL);
    o->lease_seconds = 120;
    join("Vantage Guest", NULL);
    WAIT(status().configuration == NET_ADDRESS_BOUND, 20000);
    CHECK(status().security == NET_SECURITY_OPEN);
    tick(61000);   /* T1: unicast renewal. */
    CHECK(net_peer_stats()->dhcp_acks == 2 && status().configuration == NET_ADDRESS_BOUND);
    o->nak_renewal = 1;
    tick(61000);   /* The next renewal is refused: reconfigure from scratch. */
    CHECK(net_peer_stats()->dhcp_naks >= 1);
    WAIT(status().configuration == NET_ADDRESS_BOUND, 30000);
    CHECK(net_peer_stats()->dhcp_acks >= 3);
    o->nak_renewal = 0;
    o->dhcp_silent = 1;
    uint64_t h = tcp_open(NET_PEER_WEB, 9);
    WAIT(status().configuration != NET_ADDRESS_BOUND, 130000);  /* Renew, rebind, expire. */
    CHECK(sock(1, NET_SOCKET_RECEIVE, h, 0, 0, NULL, 16, NULL) == NET_RESET);
    CHECK(sock(1, NET_SOCKET_CLOSE, h, 0, 0, NULL, 0, NULL) == 0);
    o->dhcp_silent = 0;
    WAIT(status().configuration == NET_ADDRESS_BOUND, 80000);
}

static void link_loss_and_reconnect(void) {
    connect_bound();
    uint64_t h = tcp_open(NET_PEER_WEB, 9);
    sim_net_drop_link();
    tick(5);
    NetStatus s = status();
    CHECK(s.link == NET_LINK_RETRYING && s.configuration == NET_ADDRESS_NONE && s.error == NET_OFFLINE);
    CHECK(sock(1, NET_SOCKET_RECEIVE, h, 0, 0, NULL, 16, NULL) == NET_RESET);
    CHECK(sock(1, NET_SOCKET_CLOSE, h, 0, 0, NULL, 0, NULL) == 0);
    WAIT(status().configuration == NET_ADDRESS_BOUND, 20000);
    CHECK(status().retries >= 1);
}

/* Mutated copies of real frames from the virtual network must never upset
 * the stack (run under ASan/UBSan in CI). */
static void malformed_frames(void) {
    unsigned char frames[16][1514];
    unsigned int sizes[16], count = 0;
    NetPeerOptions* o = net_peer_options();
    o->latency_ms = 1000;  /* Hold replies so they can be captured. */
    int64_t h = sock(1, NET_SOCKET_OPEN, NET_ICMP, 0, 0, NULL, 0, NULL);
    CHECK(sock(1, NET_SOCKET_CONNECT, (uint64_t)h, NET_PEER_WEB, 0, NULL, 0, NULL) == 0);
    CHECK(sock(1, NET_SOCKET_SEND, (uint64_t)h, 0, 1, "fuzz", 4, NULL) == 4);
    uint64_t tcp = (uint64_t)sock(1, NET_SOCKET_OPEN, NET_TCP, 0, 0, NULL, 0, NULL);
    (void)sock(1, NET_SOCKET_CONNECT, tcp, NET_PEER_WEB, 80, NULL, 0, NULL);
    NetResolve query = {0, 0, "fuzz.vantage.test"};
    (void)net_resolve(&query);
    now_us += 2000000;
    while (count < 16 && (sizes[count] = net_peer_output(frames[count], 1514, now_us / 1000)) != 0) count++;
    CHECK(count >= 3);
    for (unsigned int round = 0; round < 20000; round++) {
        unsigned int which = net_port_random() % count, size = sizes[which];
        unsigned char frame[1514];
        memcpy(frame, frames[which], size);
        unsigned int edits = 1 + net_port_random() % 4;
        for (unsigned int e = 0; e < edits; e++) frame[14 + net_port_random() % (size - 14)] ^= (unsigned char)(1U << (net_port_random() % 8));
        if (round % 7 == 0) size = 14 + net_port_random() % (size - 13);
        net_frame_input(frame, size);
    }
    CHECK(sock(1, NET_SOCKET_CLOSE, (uint64_t)h, 0, 0, NULL, 0, NULL) == 0);
    CHECK(sock(1, NET_SOCKET_CLOSE, tcp, 0, 0, NULL, 0, NULL) == 0);
    memset(o, 0, sizeof(*o));
    net_ip_reset();  /* Mutated ARP replies may have taught a wrong neighbour address. */
    tick(3000);
}

/* Coarse 10 ms steps for waits of minutes or hours. */
static void advance(uint64_t ms) {
    for (uint64_t i = 0; i < ms; i += 10) { now_us += 10000; net_poll(); }
}

static int64_t clock_error_ms(void) {
    NetTime t;
    CHECK(net_time(&t) == 1 && t.microseconds < 1000000U);
    return (int64_t)(t.unix_seconds * 1000ULL + t.microseconds / 1000U) - (int64_t)net_peer_unix_ms();
}

static void all_zero(const void* data, size_t bytes) {
    const unsigned char* p = data;
    for (size_t i = 0; i < bytes; i++) CHECK(!p[i]);
}

/* Withdraws the Wi-Fi address (which forgets the time) and waits for a new lease. */
static void fresh_address(void) {
    sim_net_drop_link();
    tick(5);
    NetTime t;
    memset(&t, 0xa5, sizeof(t));
    CHECK(net_time(&t) == 0);
    all_zero(&t, sizeof(t));
    WAIT(status().configuration == NET_ADDRESS_BOUND, 20000);
}

static void sntp(void) {
    NetPeerOptions* o = net_peer_options();
    const NetPeerStats* stats = net_peer_stats();
    uint64_t queries = stats->ntp_queries;
    fresh_address();
    CHECK(net_interface.ntp[0] == NET_PEER_ROUTER && !net_interface.ntp[1]);
    NetTime t;
    WAIT(net_time(&t) == 1, 3000);
    CHECK(t.synced == 1 && t.server == NET_PEER_ROUTER && t.unix_seconds >= NET_PEER_UNIX_EPOCH && !t.age_seconds);
    CHECK(stats->ntp_queries == queries + 1);
    int64_t error = clock_error_ms();
    CHECK(error >= -2 && error <= 2);
    tick(2500);
    CHECK(net_time(&t) == 1 && t.age_seconds == 2);
    error = clock_error_ms();
    CHECK(error >= -2 && error <= 2);
    /* The reply's transmit time plus half the round trip: a reply delayed
     * 300 ms on the way back reads 150 ms early. */
    o->latency_ms = 300;
    fresh_address();
    WAIT(net_time(&t) == 1, 5000);
    error = clock_error_ms();
    CHECK(error >= -152 && error <= -148);
    o->latency_ms = 0;
    /* Hourly resynchronization. */
    queries = stats->ntp_queries;
    advance(3605000);
    CHECK(stats->ntp_queries == queries + 1 && net_time(&t) == 1 && t.age_seconds < 10);
    error = clock_error_ms();
    CHECK(error >= -10 && error <= 10);
    /* After the 2036 NTP era rollover. */
    o->ntp_epoch = 2140000000ULL;
    fresh_address();
    WAIT(net_time(&t) == 1, 3000);
    CHECK(t.unix_seconds >= 2140000000ULL && t.unix_seconds < 2140100000ULL);
    error = clock_error_ms();
    CHECK(error >= -2 && error <= 2);
    o->ntp_epoch = 0;
}

static void sntp_rejections(void) {
    NetPeerOptions* o = net_peer_options();
    const NetPeerStats* stats = net_peer_stats();
    static const uint32_t faults[] = {
        NET_PEER_NTP_MODE, NET_PEER_NTP_VERSION, NET_PEER_NTP_ORIGIN, NET_PEER_NTP_UNSYNCED,
        NET_PEER_NTP_STRATUM, NET_PEER_NTP_ZERO, NET_PEER_NTP_PORT, NET_PEER_NTP_SOURCE
    };
    NetTime t;
    for (unsigned int i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        o->ntp_fault = faults[i];
        uint64_t queries = stats->ntp_queries;
        fresh_address();
        tick(6500);  /* Three requests, 2 s apart, all answered badly. */
        CHECK(net_time(&t) == 0 && stats->ntp_queries == queries + 3);
        o->ntp_fault = NET_PEER_NTP_GOOD;
        WAIT(net_time(&t) == 1, 20000);  /* After the 15 s backoff. */
        CHECK(stats->ntp_queries == queries + 4);
    }
    /* A forged reply does not end the query: the genuine one still counts. */
    o->ntp_fault = NET_PEER_NTP_ORIGIN;
    o->ntp_genuine = 1;
    uint64_t queries = stats->ntp_queries;
    fresh_address();
    WAIT(net_time(&t) == 1, 100);
    CHECK(stats->ntp_queries == queries + 1);
    o->ntp_genuine = 0;
    /* Kiss-o'-death: back off, then ask the second DHCP server. */
    o->ntp_fault = NET_PEER_NTP_KOD;
    o->ntp_web = 1;
    fresh_address();
    CHECK(net_interface.ntp[0] == NET_PEER_ROUTER && net_interface.ntp[1] == NET_PEER_WEB);
    tick(100);
    queries = stats->ntp_queries;
    tick(14000);
    CHECK(net_time(&t) == 0 && stats->ntp_queries == queries);
    WAIT(net_time(&t) == 1, 3000);
    CHECK(t.server == NET_PEER_WEB && stats->ntp_queries == queries + 1);
    o->ntp_web = 0;
    /* No answer at all: 15 s, then 30 s between queries of three requests. */
    o->ntp_fault = NET_PEER_NTP_SILENT;
    queries = stats->ntp_queries;
    fresh_address();
    tick(6500);
    CHECK(stats->ntp_queries == queries + 3);
    tick(21000);
    CHECK(stats->ntp_queries == queries + 6);
    tick(29000);
    CHECK(stats->ntp_queries == queries + 6 && net_time(&t) == 0);
    tick(1000);
    CHECK(stats->ntp_queries == queries + 7);
    o->ntp_fault = NET_PEER_NTP_GOOD;
    WAIT(net_time(&t) == 1, 3000);
    /* Without option 42 the resolver finds pool.ntp.org. */
    o->no_ntp_option = 1;
    uint64_t lookups = stats->dns_queries;
    fresh_address();
    CHECK(!net_interface.ntp[0] && !net_interface.ntp[1]);
    WAIT(net_time(&t) == 1, 3000);
    CHECK(t.server == NET_PEER_ROUTER && stats->dns_queries == lookups + 1);
    o->no_ntp_option = 0;
}

static int join_flags(const char* ssid, const char* passphrase, uint32_t flags) {
    NetJoin j;
    memset(&j, 0, sizeof(j));
    j.ssid_length = (uint32_t)strlen(ssid);
    memcpy(j.ssid, ssid, j.ssid_length);
    if (passphrase) { j.passphrase_length = (uint32_t)strlen(passphrase); memcpy(j.passphrase, passphrase, j.passphrase_length); }
    j.flags = flags;
    return net_join(&j);
}

static int has_ssid(const NetStatus* s, const char* ssid) {
    return s->ssid_length == strlen(ssid) && !memcmp(s->ssid, ssid, s->ssid_length);
}

/* Starts a scan (or shares the one running) and returns how many entries it lists. */
static unsigned int scan_all(int hidden_absent) {
    NetScan scan;
    memset(&scan, 0, sizeof(scan));
    CHECK(net_scan(&scan) == 0);
    scan.operation = NET_SCAN_RESULT;
    int result;
    WAIT((result = net_scan(&scan)) != NET_AGAIN, 2000);
    unsigned int count = 0;
    for (; result == 1; result = net_scan(&scan)) {
        count++;
        CHECK(scan.cursor == count);
        if (hidden_absent) CHECK(!(scan.ssid_length == 14 && !memcmp(scan.ssid, "Vantage Hidden", 14)));
    }
    CHECK(result == 0);
    return count;
}

static void hidden_networks(void) {
    CHECK(scan_all(1) == 3);
    CHECK(join_flags("Vantage Hidden", NET_PEER_HIDDEN_PASSPHRASE, 2) == NET_BAD);
    /* Not broadcast: only a directed probe finds it. */
    CHECK(join_flags("Vantage Hidden", NET_PEER_HIDDEN_PASSPHRASE, 0) == 0);
    WAIT(status().link == NET_LINK_DOWN, 2000);
    CHECK(status().error == NET_NOT_FOUND);
    CHECK(join_flags("Vantage Hidden", "wrong-passphrase", NET_JOIN_HIDDEN) == 0);
    CHECK(status().flags == NET_STATUS_HIDDEN);
    WAIT(status().link == NET_LINK_DOWN, 2000);
    CHECK(status().error == NET_AUTH && !status().flags);
    CHECK(join_flags("Nowhere", NULL, NET_JOIN_HIDDEN) == 0);
    WAIT(status().link == NET_LINK_DOWN, 2000);
    NetStatus s = status();
    CHECK(s.error == NET_NOT_FOUND && !s.ssid_length && !s.flags);
    CHECK(join_flags("Vantage Hidden", NET_PEER_HIDDEN_PASSPHRASE, NET_JOIN_HIDDEN) == 0);
    WAIT(status().configuration == NET_ADDRESS_BOUND, 20000);
    s = status();
    CHECK(has_ssid(&s, "Vantage Hidden") && s.flags == NET_STATUS_HIDDEN && s.security == NET_SECURITY_WPA2);
    CHECK(s.rssi == -55 && s.address == NET_PEER_CLIENT && s.link == NET_LINK_UP);
    uint32_t address = 0;
    CHECK(resolve("vantage.test", &address, 3000) == 1 && address == NET_PEER_WEB);
    CHECK(scan_all(1) == 3);
    /* A directed probe also finds a network that does broadcast. */
    CHECK(join_flags("VantageNet", NET_PEER_PASSPHRASE, NET_JOIN_HIDDEN) == 0);
    WAIT(status().configuration == NET_ADDRESS_BOUND, 20000);
    CHECK(status().flags == NET_STATUS_HIDDEN);
    connect_bound();
    CHECK(!status().flags);
}

static int profile(uint32_t operation, const char* ssid, const char* passphrase, uint32_t flags) {
    NetProfile p;
    memset(&p, 0, sizeof(p));
    p.operation = operation;
    p.flags = flags;
    if (ssid) { p.ssid_length = (uint32_t)strlen(ssid); memcpy(p.ssid, ssid, p.ssid_length); }
    if (passphrase) { p.passphrase_length = (uint32_t)strlen(passphrase); memcpy(p.passphrase, passphrase, p.passphrase_length); }
    return net_profile(&p);
}

static void profiles_crud(void) {
    CHECK(profile(NET_PROFILE_CLEAR, NULL, NULL, 0) == 0 && !net_profile_count());
    CHECK(profile(NET_PROFILE_ADD, "x", NULL, 2) == NET_BAD);
    CHECK(profile(NET_PROFILE_ADD, "", NULL, 0) == NET_BAD);
    CHECK(profile(NET_PROFILE_ADD, "x", "short", 0) == NET_BAD);
    CHECK(profile(NET_PROFILE_ADD, "x", "tab\tpassphrase", 0) == NET_BAD);
    CHECK(profile(9, "x", NULL, 0) == NET_BAD);
    CHECK(profile(NET_PROFILE_REMOVE, "", NULL, 0) == NET_BAD);
    NetProfile big;
    memset(&big, 0, sizeof(big));
    big.ssid_length = NET_SSID_MAX + 1;
    CHECK(net_profile(&big) == NET_BAD);
    for (unsigned int i = 0; i < NET_PROFILE_LIMIT; i++) {
        char name[8];
        snprintf(name, sizeof(name), "net%u", i);
        CHECK(profile(NET_PROFILE_ADD, name, i % 2 ? "passphrase-1" : NULL, i == 3 ? NET_PROFILE_HIDDEN : 0) == 0);
    }
    CHECK(profile(NET_PROFILE_ADD, "net8", NULL, 0) == NET_NO_SPACE);
    CHECK(profile(NET_PROFILE_ADD, "net2", "replacement", NET_PROFILE_HIDDEN) == 0);  /* Replaces. */
    CHECK(net_profile_count() == NET_PROFILE_LIMIT);
    NetProfile list;
    memset(&list, 0x5a, sizeof(list));
    list.operation = NET_PROFILE_LIST;
    list.cursor = 0;
    for (unsigned int i = 0; i < NET_PROFILE_LIMIT; i++) {
        char name[8];
        snprintf(name, sizeof(name), "net%u", i);
        CHECK(net_profile(&list) == 1 && list.operation == NET_PROFILE_LIST && list.cursor == i + 1);
        CHECK(list.ssid_length == 4 && !memcmp(list.ssid, name, 4) && !list.reserved);
        CHECK(list.passphrase_length == (i % 2 || i == 2 ? 1U : 0U));
        CHECK(list.flags == (i == 2 || i == 3 ? NET_PROFILE_HIDDEN : 0U));
        all_zero(list.passphrase, sizeof(list.passphrase));
        all_zero(list.ssid + 4, sizeof(list.ssid) - 4);
    }
    CHECK(net_profile(&list) == 0);
    CHECK(profile(NET_PROFILE_REMOVE, "net9", NULL, 0) == NET_NOT_FOUND);
    CHECK(profile(NET_PROFILE_REMOVE, "net1", NULL, 0) == 0 && net_profile_count() == NET_PROFILE_LIMIT - 1);
    const NetSavedProfile* table = net_profile_at(0);
    all_zero(&table[NET_PROFILE_LIMIT - 1], sizeof(table[0]));  /* Passphrases do not linger. */
    CHECK(net_profile_at(1)->ssid_bytes == 4 && !memcmp(net_profile_at(1)->ssid, "net2", 4));
    CHECK(profile(NET_PROFILE_CLEAR, NULL, NULL, 0) == 0 && !net_profile_count() && !net_profile_at(0));
    all_zero(table, sizeof(table[0]) * NET_PROFILE_LIMIT);
    list.cursor = 0;
    CHECK(net_profile(&list) == 0);
    NetStatus s = status();
    CHECK(s.link == NET_LINK_UP && has_ssid(&s, "VantageNet") && !s.flags);  /* Undisturbed. */
}

static void profile_up(const char* ssid, unsigned int budget) {
    WAIT(status().configuration == NET_ADDRESS_BOUND && status().link == NET_LINK_UP, budget);
    NetStatus s = status();
    CHECK(has_ssid(&s, ssid) && (s.flags & NET_STATUS_PROFILE) && !s.error);
}

static void autojoin(void) {
    const NetPeerStats* stats = net_peer_stats();
    NetPeerOptions* o = net_peer_options();
    CHECK(net_leave() == 0);
    /* Adding resumes; the strongest compatible network goes first and the
     * WPA3-only one is never tried. A user scan shares the automatic one. */
    uint64_t joins = stats->joins;
    CHECK(profile(NET_PROFILE_ADD, "Neighbour WPA3", "passphrase-1", 0) == 0);
    CHECK(profile(NET_PROFILE_ADD, "Vantage Guest", NULL, 0) == 0);
    CHECK(profile(NET_PROFILE_ADD, "VantageNet", NET_PEER_PASSPHRASE, 0) == 0);
    tick(1);
    CHECK(scan_all(0) == 3);
    profile_up("VantageNet", 20000);
    CHECK(status().flags == NET_STATUS_PROFILE && stats->joins == joins + 1);
    /* LEAVE pauses automatic connection. */
    CHECK(net_leave() == 0);
    joins = stats->joins;
    advance(120000);
    NetStatus s = status();
    CHECK(s.link == NET_LINK_DOWN && !s.ssid_length && stats->joins == joins);
    /* A failing candidate falls through to the next. */
    CHECK(profile(NET_PROFILE_ADD, "VantageNet", "wrong-passphrase", 0) == 0);
    profile_up("Vantage Guest", 20000);
    CHECK(stats->joins == joins + 2 && net_profile_count() == 3);
    /* An explicit join takes over from an automatic scan, and from an
     * automatic join in progress. */
    CHECK(net_leave() == 0);
    CHECK(profile(NET_PROFILE_ADD, "VantageNet", NET_PEER_PASSPHRASE, 0) == 0);
    tick(100);
    joins = stats->joins;
    join("Vantage Guest", NULL);
    WAIT(status().configuration == NET_ADDRESS_BOUND, 20000);
    tick(2000);
    s = status();
    CHECK(has_ssid(&s, "Vantage Guest") && !s.flags && stats->joins == joins + 1);
    CHECK(net_leave() == 0);
    CHECK(profile(NET_PROFILE_ADD, "VantageNet", NET_PEER_PASSPHRASE, 0) == 0);
    WAIT(status().link == NET_LINK_JOINING, 2000);
    CHECK(status().flags == NET_STATUS_PROFILE);
    join("Vantage Guest", NULL);
    WAIT(status().configuration == NET_ADDRESS_BOUND, 20000);
    s = status();
    CHECK(has_ssid(&s, "Vantage Guest") && !s.flags);
    /* A failed explicit join erases its own credentials, never a profile;
     * automatic connection resumes after 30 s. */
    CHECK(net_leave() == 0);
    join("VantageNet", "wrong-passphrase");
    WAIT(status().link == NET_LINK_DOWN, 2000);
    s = status();
    CHECK(s.error == NET_AUTH && !s.ssid_length && net_profile_count() == 3);
    joins = stats->joins;
    advance(25000);
    CHECK(status().link == NET_LINK_DOWN && stats->joins == joins);
    profile_up("VantageNet", 10000);
    /* Hidden profiles are probed after the visible candidates. */
    CHECK(net_leave() == 0);
    CHECK(profile(NET_PROFILE_CLEAR, NULL, NULL, 0) == 0);
    CHECK(profile(NET_PROFILE_ADD, "Vantage Hidden", NET_PEER_HIDDEN_PASSPHRASE, NET_PROFILE_HIDDEN) == 0);
    CHECK(profile(NET_PROFILE_ADD, "VantageNet", "wrong-passphrase", 0) == 0);
    joins = stats->joins;
    profile_up("Vantage Hidden", 20000);
    CHECK(status().flags == (NET_STATUS_PROFILE | NET_STATUS_HIDDEN) && stats->joins == joins + 2);
    /* A saved network that disappears is retried three times, then the
     * next one is chosen from a new scan. */
    CHECK(net_leave() == 0);
    CHECK(profile(NET_PROFILE_CLEAR, NULL, NULL, 0) == 0);
    CHECK(profile(NET_PROFILE_ADD, "Vantage Guest", NULL, 0) == 0);
    CHECK(profile(NET_PROFILE_ADD, "VantageNet", NET_PEER_PASSPHRASE, 0) == 0);
    profile_up("VantageNet", 20000);
    o->aps_off = NET_PEER_AP_MAIN;
    joins = stats->joins;
    sim_net_drop_link();
    tick(5);
    s = status();
    CHECK(s.link == NET_LINK_RETRYING && has_ssid(&s, "VantageNet") && s.flags == NET_STATUS_PROFILE);
    profile_up("Vantage Guest", 60000);
    CHECK(stats->joins == joins + 4);
    o->aps_off = 0;
    /* Nothing joinable: wait 30 s, then 60 s, between attempts. */
    CHECK(net_leave() == 0);
    CHECK(profile(NET_PROFILE_CLEAR, NULL, NULL, 0) == 0);
    joins = stats->joins;
    CHECK(profile(NET_PROFILE_ADD, "Nowhere", NULL, NET_PROFILE_HIDDEN) == 0);
    tick(1500);
    CHECK(stats->joins == joins + 1 && status().link == NET_LINK_DOWN && status().error == NET_NOT_FOUND);
    advance(28000);
    CHECK(stats->joins == joins + 1);
    advance(4000);
    CHECK(stats->joins == joins + 2);
    advance(55000);
    CHECK(stats->joins == joins + 2);
    advance(10000);
    CHECK(stats->joins == joins + 3);
    /* Adding a profile rescans at once. */
    CHECK(profile(NET_PROFILE_ADD, "Vantage Guest", NULL, 0) == 0);
    profile_up("Vantage Guest", 3000);
    CHECK(profile(NET_PROFILE_CLEAR, NULL, NULL, 0) == 0);
    CHECK(net_leave() == 0);
    connect_bound();
}

static const unsigned char wired_mac[6] = {0x02, 0x56, 0x41, 0x4e, 0x54, 0x02};

static void ethernet_switching(void) {
    const NetPeerStats* stats = net_peer_stats();
    NetStatus s = status();
    CHECK(s.interface == NET_INTERFACE_WIFI && s.address == NET_PEER_CLIENT && s.ethernet == NET_ETHERNET_NO_CABLE);
    uint64_t wireless = tcp_open(NET_PEER_WEB, 9);
    sim_net_set_cable(1);
    WAIT(status().interface == NET_INTERFACE_ETHERNET, 1000);
    CHECK(sock(1, NET_SOCKET_RECEIVE, wireless, 0, 0, NULL, 16, NULL) == NET_RESET);
    CHECK(sock(1, NET_SOCKET_CLOSE, wireless, 0, 0, NULL, 0, NULL) == 0);
    WAIT(status().configuration == NET_ADDRESS_BOUND, 20000);
    s = status();
    CHECK(s.address == NET_PEER_CLIENT + 1 && s.gateway == NET_PEER_ROUTER && s.dns == NET_PEER_ROUTER);
    CHECK(s.ethernet == NET_ETHERNET_UP && s.ethernet_mbps == 1000);
    CHECK(s.link == NET_LINK_UP && has_ssid(&s, "VantageNet") && s.rssi == -42);  /* Wi-Fi stays associated. */
    CHECK(!memcmp(net_interface.mac, wired_mac, 6));
    uint32_t address = 0;
    CHECK(resolve("www.vantage.test", &address, 3000) == 1 && address == NET_PEER_WEB);
    echo_transfer(NET_PEER_REMOTE, 20000);
    NetTime t;
    WAIT(net_time(&t) == 1, 3000);
    /* Wi-Fi drops and recovers in the background without touching IPv4. */
    uint64_t wired = tcp_open(NET_PEER_WEB, 9);
    uint64_t releases = stats->dhcp_releases;
    sim_net_drop_link();
    tick(5);
    s = status();
    CHECK(s.link == NET_LINK_RETRYING && s.interface == NET_INTERFACE_ETHERNET && s.address == NET_PEER_CLIENT + 1);
    WAIT(status().link == NET_LINK_UP, 10000);
    CHECK(sock(1, NET_SOCKET_STATUS, wired, 0, 0, NULL, 0, NULL) == NET_TCP_ESTABLISHED);
    CHECK(scan_all(1) == 3);
    /* Unplugged: back to Wi-Fi, with its own lease and MAC. */
    sim_net_set_cable(0);
    WAIT(status().interface == NET_INTERFACE_WIFI, 1000);
    s = status();
    CHECK(s.ethernet == NET_ETHERNET_NO_CABLE && !s.ethernet_mbps && net_time(&t) == 0);
    CHECK(sock(1, NET_SOCKET_RECEIVE, wired, 0, 0, NULL, 16, NULL) == NET_RESET);
    CHECK(sock(1, NET_SOCKET_CLOSE, wired, 0, 0, NULL, 0, NULL) == 0);
    WAIT(status().configuration == NET_ADDRESS_BOUND, 20000);
    CHECK(status().address == NET_PEER_CLIENT && memcmp(net_interface.mac, wired_mac, 6));
    echo_transfer(NET_PEER_WEB, 20000);
    /* Leaving Wi-Fi while Ethernet carries IPv4 keeps the wired lease. */
    sim_net_set_cable(1);
    WAIT(status().interface == NET_INTERFACE_ETHERNET && status().configuration == NET_ADDRESS_BOUND, 20000);
    CHECK(net_leave() == 0);
    tick(20);
    s = status();
    CHECK(s.link == NET_LINK_DOWN && s.interface == NET_INTERFACE_ETHERNET && s.address == NET_PEER_CLIENT + 1);
    CHECK(stats->dhcp_releases == releases);
    sim_net_set_cable(0);
    WAIT(status().interface == NET_INTERFACE_NONE, 1000);
    s = status();
    CHECK(!s.address && s.configuration == NET_ADDRESS_NONE && s.ethernet == NET_ETHERNET_NO_CABLE);
    CHECK(resolve("vantage.test", &address, 1) == NET_OFFLINE);
    connect_bound();
    CHECK(status().interface == NET_INTERFACE_WIFI && !used_sockets());
}

static void new_syscalls(void) {
    NetTime t;
    WAIT(net_time(&t) == 1, 3000);
    NetTime* time_out = (NetTime*)arena.bytes;
    memset(time_out, 0xa5, sizeof(*time_out));
    CHECK(handle_net_call(SYS_NET_TIME, (uintptr_t)time_out, 1) == 1);
    CHECK(time_out->synced == 1 && time_out->server == NET_PEER_ROUTER && time_out->microseconds < 1000000U);
    CHECK(handle_net_call(SYS_NET_TIME, (uintptr_t)(arena.bytes + 4), 1) == NET_BAD);
    CHECK(handle_net_call(SYS_NET_TIME, (uintptr_t)(arena.bytes + sizeof(arena.bytes) - 8), 1) == NET_BAD);
    CHECK(handle_net_call(SYS_NET_TIME, (uintptr_t)&t, 1) == NET_BAD);
    CHECK(handle_net_call(SYS_NET_TIME, (uintptr_t)time_out, 0) == NET_UNAVAILABLE);

    NetProfile* p = (NetProfile*)arena.bytes;
    memset(p, 0, sizeof(*p));
    p->operation = NET_PROFILE_ADD;
    p->ssid_length = 10; memcpy(p->ssid, "VantageNet", 10);
    p->passphrase_length = 12; memcpy(p->passphrase, NET_PEER_PASSPHRASE, 12);
    CHECK(handle_net_call(SYS_NET_PROFILE, (uintptr_t)p, 1) == 0 && net_profile_count() == 1);
    memset(p, 'x', sizeof(*p));
    p->operation = NET_PROFILE_LIST; p->cursor = 0;
    CHECK(handle_net_call(SYS_NET_PROFILE, (uintptr_t)p, 1) == 1);
    CHECK(p->cursor == 1 && p->ssid_length == 10 && p->passphrase_length == 1 && !p->flags);
    all_zero(p->passphrase, sizeof(p->passphrase));
    CHECK(handle_net_call(SYS_NET_PROFILE, (uintptr_t)p, 1) == 0);
    p->operation = 9;
    CHECK(handle_net_call(SYS_NET_PROFILE, (uintptr_t)p, 1) == NET_BAD);
    p->operation = NET_PROFILE_LIST; p->cursor = 0;
    CHECK(handle_net_call(SYS_NET_PROFILE, (uintptr_t)(arena.bytes + 2), 1) == NET_BAD);
    CHECK(handle_net_call(SYS_NET_PROFILE, (uintptr_t)(arena.bytes + sizeof(arena.bytes) - 64), 1) == NET_BAD);
    memset(p, 0, sizeof(*p));
    p->operation = NET_PROFILE_CLEAR;
    CHECK(handle_net_call(SYS_NET_PROFILE, (uintptr_t)p, 1) == 0 && !net_profile_count());

    NetRandom* r = (NetRandom*)arena.bytes;
    memset(r, 0, sizeof(*r));
    CHECK(handle_net_call(SYS_NET_RANDOM, (uintptr_t)r, 1) == NET_BAD);
    r->length = NET_RANDOM_MAX + 1;
    CHECK(handle_net_call(SYS_NET_RANDOM, (uintptr_t)r, 1) == NET_BAD);
    r->length = 16; r->flags = 1;
    CHECK(handle_net_call(SYS_NET_RANDOM, (uintptr_t)r, 1) == NET_BAD);
    r->flags = 0;
    CHECK(handle_net_call(SYS_NET_RANDOM, (uintptr_t)(arena.bytes + 2), 1) == NET_BAD);
    CHECK(handle_net_call(SYS_NET_RANDOM, (uintptr_t)r, 1) == 16);
    unsigned int nonzero = 0;
    for (unsigned int i = 0; i < 16; i++) nonzero += r->data[i] != 0;
    CHECK(nonzero > 8);
    all_zero(r->data + 16, NET_RANDOM_MAX - 16);
}

static unsigned int wifi_injections;

/* An ARP request from the router for our current address, arriving over Wi-Fi. */
static int injecting_receive(void* context, void* data, unsigned int capacity) {
    if (!wifi_injections) return sim_net_link()->receive(context, data, capacity);
    static const unsigned char router_mac[6] = {0x02, 0x56, 0x4e, 0x00, 0x00, 0x01};
    unsigned char* f = data;
    CHECK(capacity >= 60);
    memset(f, 0, 60);
    memset(f, 0xff, 6);
    memcpy(f + 6, router_mac, 6);
    net_put16(f + 12, 0x0806);
    net_put16(f + 14, 1); net_put16(f + 16, 0x0800); f[18] = 6; f[19] = 4; net_put16(f + 20, 1);
    memcpy(f + 22, router_mac, 6); net_put32(f + 28, NET_PEER_ROUTER);
    net_put32(f + 38, net_interface.address);
    wifi_injections--;
    return 60;
}

static void inactive_link_frames(void) {
    static NetLink wifi;
    wifi = *sim_net_link();
    wifi.receive = injecting_receive;
    net_attach(&wifi, sim_net_ethernet_link());
    connect_bound();
    const NetPeerStats* stats = net_peer_stats();
    uint64_t replies = stats->arp_replies;
    wifi_injections = 1;
    tick(5);
    CHECK(stats->arp_replies == replies + 1);  /* Answered while Wi-Fi is active. */
    sim_net_set_cable(1);
    WAIT(status().interface == NET_INTERFACE_ETHERNET && status().configuration == NET_ADDRESS_BOUND, 20000);
    tick(100);
    replies = stats->arp_replies;
    uint64_t frames = net_interface.rx_frames;
    wifi_injections = 1;
    tick(5);
    CHECK(!wifi_injections && stats->arp_replies == replies && net_interface.rx_frames == frames);
    sim_net_set_cable(0);
    WAIT(status().interface == NET_INTERFACE_WIFI && status().configuration == NET_ADDRESS_BOUND, 20000);
}

static int ethernet_broken;
static int broken_poll(void* context) { return ethernet_broken ? NET_IO : sim_net_ethernet_link()->poll(context); }

static void ethernet_alone(void) {
    static NetLink wired;
    wired = *sim_net_ethernet_link();
    wired.poll = broken_poll;
    net_attach(NULL, &wired);
    sim_net_set_cable(1);
    WAIT(status().configuration == NET_ADDRESS_BOUND, 20000);
    NetStatus s = status();
    CHECK(!s.available && !s.link && s.interface == NET_INTERFACE_ETHERNET && s.ethernet == NET_ETHERNET_UP);
    CHECK(s.address == NET_PEER_CLIENT + 1 && !s.ssid_length);
    NetScan scan = {0};
    CHECK(net_scan(&scan) == NET_UNAVAILABLE && net_leave() == NET_UNAVAILABLE);
    CHECK(join_flags("VantageNet", NET_PEER_PASSPHRASE, 0) == NET_UNAVAILABLE);
    CHECK(profile(NET_PROFILE_ADD, "VantageNet", NULL, 0) == NET_UNAVAILABLE);
    uint32_t address = 0;
    CHECK(resolve("vantage.test", &address, 3000) == 1 && address == NET_PEER_WEB);
    NetTime t;
    WAIT(net_time(&t) == 1, 3000);
    ethernet_broken = 1;
    tick(2);
    s = status();
    CHECK(s.ethernet == NET_ETHERNET_ABSENT && s.interface == NET_INTERFACE_NONE && !s.address && !s.configuration);
    CHECK(resolve("vantage.test", &address, 1) == NET_UNAVAILABLE && net_time(&t) == 0);
    ethernet_broken = 0;
    tick(1000);
    CHECK(status().ethernet == NET_ETHERNET_ABSENT);  /* Failed for the rest of the boot. */
    sim_net_set_cable(0);
}

int main(void) {
    net_attach(NULL, NULL);
    PHASE(unavailable_and_literals);
    sim_net_attach();
    CHECK(status().available && status().link == NET_LINK_DOWN);
    PHASE(scan_and_join_failures);
    PHASE(bound_configuration);
    PHASE(icmp);
    PHASE(dns);
    PHASE(udp);
    PHASE(http);
    PHASE(refused);
    PHASE(lossy_transfers);
    PHASE(spurious_timeout);
    PHASE(zero_window);
    PHASE(timeouts);
    PHASE(ownership_and_limits);
    PHASE(syscall_boundary);
    PHASE(malformed_frames);
    PHASE(dhcp_lifecycle);
    PHASE(link_loss_and_reconnect);
    PHASE(sntp);
    PHASE(sntp_rejections);
    PHASE(hidden_networks);
    PHASE(profiles_crud);
    PHASE(autojoin);
    PHASE(ethernet_switching);
    PHASE(new_syscalls);
    PHASE(inactive_link_frames);
    CHECK(!net_peer_stats()->invalid);
    PHASE(ethernet_alone);
    CHECK(!net_peer_stats()->invalid);
    puts("PASS: scan/join/DHCP lifecycle, ARP, ICMP, DNS (CNAME, spoof, loop, TTL), UDP errors, TCP "
         "HTTP/echo/chargen under loss, duplication and reordering, spurious timeouts, zero window, timeouts, link loss, "
         "socket ownership, syscall copies and mutated frames");
    puts("PASS: SNTP (offset, resync, era, rejected replies, kiss-o'-death, backoff, option 42 and pool), hidden "
         "networks, profiles and automatic connection, Ethernet switching and failure, time/profile/random syscalls");
    return 0;
}
