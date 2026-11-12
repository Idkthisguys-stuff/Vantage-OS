/* The TLS client (user/lib/tls.c) and the real user library wrappers over the
 * production network stack, against the virtual network's HTTPS hosts
 * (sim/platform/tls_peer.c), with handshakes, certificate failures, closure,
 * truncation, every offered cipher suite and lossy multi-record transfers. */
#include "sim/platform/net_peer.h"
#include "sim/platform/net_sim.h"
#include "sim/platform/tls_peer.h"
#include "user/lib/tls_trust.h"
#include <vantage/kernel/syscall.h>
#include <vantage/tls.h>
#include <vantage/user.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* phase = "start";
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s, line %d: %s\n", phase, __LINE__, #c); exit(1); } } while (0)
#define PHASE(f) (phase = #f, f())

static uint64_t now_us;
static uint64_t seed = 0x9e3779b97f4a7c15ULL;
static int entropy_off;

uint32_t time_us(void) { return (uint32_t)now_us; }
uint32_t net_port_random(void) {
    seed ^= seed >> 12; seed ^= seed << 25; seed ^= seed >> 27;
    return (uint32_t)((seed * 0x2545f4914f6cdd1dULL) >> 32);
}
int net_entropy(void* data, unsigned int bytes) {
    if (entropy_off) return NET_UNAVAILABLE;
    for (unsigned int i = 0; i < bytes; i++) ((unsigned char*)data)[i] = (unsigned char)net_port_random();
    return 0;
}
/* The user library's requests live on this process's stack: all valid. */
uint32_t user_buffer_valid(uintptr_t buffer, size_t size) { (void)buffer; (void)size; return 1; }

/* The simulator transport: the library's network calls go to the kernel's
 * syscall layer as process 1. */
uint64_t handle_sys_call(uint64_t id, uint64_t argument) {
    CHECK(id >= SYS_NET_STATUS && id <= SYS_NET_RANDOM);
    return (uint64_t)handle_net_call(id, (uintptr_t)argument, 1);
}

static void tick(unsigned int ms) {
    for (unsigned int i = 0; i < ms; i++) { now_us += 1000; net_poll(); }
}

static const char page[] =
    "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nServer: vantage-virtual-network\r\n\r\n"
    "Hello from the Vantage virtual network!\n"
    "This page was served over TCP by the simulator's web host.\n";

static char body[32768];

static uint64_t tcp_open(uint32_t address, uint16_t port) {
    int64_t handle = sys_net_open(NET_TCP);
    int result;
    unsigned int waited = 0;
    CHECK(handle > 0);
    while ((result = sys_net_connect((uint64_t)handle, address, port)) == NET_AGAIN) { CHECK(waited++ < 70000); tick(1); }
    CHECK(result == 0);
    return (uint64_t)handle;
}

static int handshake(void) {
    int result;
    unsigned int waited = 0;
    while ((result = tls_handshake()) == NET_AGAIN) { CHECK(waited++ < 60000); tick(1); }
    return result;
}

/* Sends a GET for path and reads until the session ends: the received length
 * is stored in *length; returns the final tls_receive result (0 or an error). */
static int get(const char* path, unsigned int* length) {
    char request[128];
    int n = snprintf(request, sizeof(request), "GET %s HTTP/1.0\r\nHost: vantage.test\r\n\r\n", path);
    unsigned int sent = 0, idle = 0;
    while (sent < (unsigned int)n) {
        int result = tls_send(request + sent, (uint32_t)n - sent);
        if (result == NET_AGAIN) { tick(1); continue; }
        CHECK(result > 0);
        sent += (unsigned int)result;
    }
    *length = 0;
    for (;;) {
        int result = tls_receive(body + *length, (uint32_t)(sizeof(body) - 1 - *length));
        if (result == NET_AGAIN) { CHECK(++idle < 60000); tick(1); continue; }
        if (result <= 0) { body[*length] = 0; return result; }
        idle = 0;
        *length += (unsigned int)result;
        CHECK(*length < sizeof(body) - 1);
    }
}

static void finish(uint64_t handle) {
    tls_close();
    CHECK(sys_net_close(handle) == 0);
    tick(3000);
}

/* A complete HTTPS GET; returns the response length in body. */
static unsigned int fetch(uint32_t address, const char* host, const char* path) {
    uint64_t h = tcp_open(address, 443);
    unsigned int length;
    CHECK(tls_open(h, host) == 0);
    CHECK(handshake() == 1);
    CHECK(get(path, &length) == 0);
    finish(h);
    return length;
}

/* The plain HTTP response for comparison. */
static unsigned int fetch_plain(const char* path, char* out, unsigned int capacity) {
    uint64_t h = tcp_open(NET_PEER_WEB, 80);
    char request[128];
    int n = snprintf(request, sizeof(request), "GET %s HTTP/1.0\r\n\r\n", path);
    unsigned int length = 0;
    CHECK(sys_net_send(h, request, (uint32_t)n) == n);
    for (;;) {
        int got = sys_net_receive(h, out + length, capacity - length);
        if (got == NET_AGAIN) { tick(1); continue; }
        CHECK(got >= 0);
        if (!got) break;
        length += (unsigned int)got;
        CHECK(length < capacity);
    }
    CHECK(sys_net_close(h) == 0);
    tick(3000);
    return length;
}

static void connect_bound(void) {
    NetStatus s;
    CHECK(sys_net_join("VantageNet", NET_PEER_PASSPHRASE, 0) == 0);
    for (unsigned int waited = 0;; waited++) {
        CHECK(sys_net_status(&s) == 0);
        if (s.configuration == NET_ADDRESS_BOUND) break;
        CHECK(waited < 20000);
        tick(1);
    }
}

static void argument_checks(void) {
    char buffer[8];
    CHECK(tls_handshake() == NET_BAD && tls_send("x", 1) == NET_BAD && tls_receive(buffer, 8) == NET_BAD);
    tls_close();
    CHECK(tls_open(1, NULL) == NET_BAD && tls_open(1, "") == NET_BAD);
    char long_name[NET_NAME_MAX + 1];
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[NET_NAME_MAX] = 0;
    CHECK(tls_open(1, long_name) == NET_BAD);
    long_name[NET_NAME_MAX - 1] = 0;
    CHECK(tls_open(1, long_name) == 0);  /* Longest accepted name; no I/O yet. */
    tls_close();
    CHECK(!strcmp(tls_error_text(TLS_CERTIFICATE), "Server certificate rejected"));
    CHECK(!strcmp(tls_error_text(NET_RESET), "Connection reset") && !strcmp(tls_error_text(12345), "Unknown error"));
}

static void https_get(void) {
    unsigned int length = fetch(NET_PEER_WEB, "vantage.test", "/");
    CHECK(length == sizeof(page) - 1 && !memcmp(body, page, length));
    /* The same certificate is served by the remote host. */
    length = fetch(NET_PEER_REMOTE, "vantage.test", "/missing");
    CHECK(strstr(body, "HTTP/1.0 404 Not Found") == body && length == strlen(body));
}

static void certificate_failures(void) {
    uint64_t h = tcp_open(NET_PEER_REMOTE, 443);
    CHECK(tls_open(h, "remote.vantage.test") == 0);
    CHECK(handshake() == TLS_CERTIFICATE);
    char buffer[8];
    CHECK(tls_receive(buffer, sizeof(buffer)) == TLS_CERTIFICATE && tls_send("x", 1) == TLS_CERTIFICATE);
    finish(h);

    size_t count;
    const br_x509_trust_anchor* real = tls_sim_trust_anchors(&count);
    CHECK(count == 1);
    tls_set_trust_anchors(real, 0);
    CHECK(tls_open(1, "vantage.test") == TLS_NO_TRUST);

    static const unsigned char other_dn[] = {0x30, 0x0f, 0x31, 0x0d, 0x30, 0x0b, 0x06, 0x03, 0x55, 0x04,
                                             0x03, 0x0c, 0x04, 'N', 'o', 'n', 'e'};
    static unsigned char bad_point[160];
    br_x509_trust_anchor wrong[2];
    wrong[0] = wrong[1] = *real;
    wrong[0].dn.data = (unsigned char*)other_dn;
    wrong[0].dn.len = sizeof(other_dn);
    CHECK(real->pkey.key.ec.qlen <= sizeof(bad_point));
    memcpy(bad_point, real->pkey.key.ec.q, real->pkey.key.ec.qlen);
    bad_point[real->pkey.key.ec.qlen - 1] ^= 1;
    wrong[1].pkey.key.ec.q = bad_point;
    for (unsigned int i = 0; i < 2; i++) {
        tls_set_trust_anchors(&wrong[i], 1);
        h = tcp_open(NET_PEER_WEB, 443);
        CHECK(tls_open(h, "vantage.test") == 0);
        CHECK(handshake() == TLS_CERTIFICATE);
        finish(h);
    }
    tls_set_trust_anchors(NULL, 0);
    CHECK(fetch(NET_PEER_WEB, "vantage.test", "/") == sizeof(page) - 1);
}

static void entropy_and_busy(void) {
    entropy_off = 1;
    CHECK(tls_open(1, "vantage.test") == TLS_NO_RANDOM);
    entropy_off = 0;
    uint64_t h = tcp_open(NET_PEER_WEB, 443);
    CHECK(tls_open(h, "vantage.test") == 0);
    CHECK(tls_open(h, "vantage.test") == TLS_BUSY);
    CHECK(handshake() == 1);
    unsigned int length;
    CHECK(get("/", &length) == 0 && length == sizeof(page) - 1);
    char buffer[8];
    CHECK(tls_receive(buffer, sizeof(buffer)) == 0);  /* close_notify stays an orderly end. */
    CHECK(tls_send("x", 1) < 0);
    finish(h);
}

static void truncation(void) {
    tls_peer_options()->truncate = 1;
    uint64_t h = tcp_open(NET_PEER_WEB, 443);
    unsigned int length;
    CHECK(tls_open(h, "vantage.test") == 0 && handshake() == 1);
    CHECK(get("/", &length) == TLS_PROTOCOL);
    CHECK(length == sizeof(page) - 1 && !memcmp(body, page, length));
    finish(h);
    tls_peer_options()->truncate = 0;
}

static void cipher_suites(void) {
    static const struct { uint16_t suite; int rsa; } offered[] = {
        {BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, 0}, {BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, 0},
        {BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, 0}, {BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256, 1},
        {BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, 1}, {BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384, 1},
    };
    TlsPeerOptions* o = tls_peer_options();
    for (unsigned int i = 0; i < sizeof(offered) / sizeof(offered[0]); i++) {
        o->suite = offered[i].suite;
        o->rsa = offered[i].rsa;
        CHECK(fetch(NET_PEER_WEB, "vantage.test", "/") == sizeof(page) - 1 && !memcmp(body, page, sizeof(page) - 1));
    }
    o->suite = BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256;  /* Not offered by the client. */
    o->rsa = 0;
    uint64_t h = tcp_open(NET_PEER_WEB, 443);
    CHECK(tls_open(h, "vantage.test") == 0 && handshake() == TLS_PROTOCOL);
    finish(h);
    memset(o, 0, sizeof(*o));
}

static void large_transfers(void) {
    static char plain[32768];
    unsigned int expected = fetch_plain("/large", plain, sizeof(plain));
    CHECK(expected > 20000);
    TlsPeerOptions* o = tls_peer_options();
    NetPeerOptions* network = net_peer_options();
    o->record_bytes = 512;  /* About 40 records. */
    CHECK(fetch(NET_PEER_WEB, "vantage.test", "/large") == expected && !memcmp(body, plain, expected));
    /* As in network_stack: the virtual network's drops are periodic, and its
     * go-back-N sender can lose the same segment of every burst for some
     * periods, which no client recovers from. */
    network->latency_ms = 20; network->drop_up = 7; network->drop_down = 11;
    network->duplicate_down = 5; network->reorder_down = 13;
    o->record_bytes = 1000;
    uint64_t retransmits = net_peer_stats()->tcp_retransmits, dropped = net_peer_stats()->dropped;
    CHECK(fetch(NET_PEER_REMOTE, "vantage.test", "/large") == expected && !memcmp(body, plain, expected));
    CHECK(net_peer_stats()->tcp_retransmits > retransmits && net_peer_stats()->dropped > dropped);
    memset(network, 0, sizeof(*network));
    memset(o, 0, sizeof(*o));
    tick(3000);
}

int main(void) {
    net_attach(NULL, NULL);
    sim_net_attach();
    connect_bound();
    PHASE(argument_checks);
    PHASE(https_get);
    PHASE(certificate_failures);
    PHASE(entropy_and_busy);
    PHASE(truncation);
    PHASE(cipher_suites);
    PHASE(large_transfers);
    CHECK(!net_peer_stats()->invalid);
    puts("PASS: HTTPS GET over TLS 1.2, name mismatch, missing and wrong trust anchors, missing entropy, busy "
         "session, close_notify, truncation, all six cipher suites with EC and RSA keys, and multi-record "
         "transfers under loss, duplication, reordering and latency");
    return 0;
}
