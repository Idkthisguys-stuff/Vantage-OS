/* Simulated station and wired port: association and scans complete after
 * short delays and frames are exchanged with the virtual network. Simulator
 * only. */
#include "net_sim.h"
#include "net_peer.h"
#include <vantage/kernel/platform.h>

#define JOIN_MS 400U
#define SCAN_MS 800U

static struct {
    int joining, connected, result, scanning, scanned, hidden;
    int32_t rssi;
    uint64_t join_at, scan_at;
    unsigned int ssid_bytes, passphrase_bytes;
    unsigned char ssid[NET_SSID_MAX], passphrase[64];
} station;
static int cable;
static struct { int started; uint32_t last; uint64_t remainder, ms; } clock;
static const unsigned char mac[6] = {0x02, 0x56, 0x41, 0x4e, 0x54, 0x01};
static const unsigned char wired_mac[6] = {0x02, 0x56, 0x41, 0x4e, 0x54, 0x02};

static uint64_t now_ms(void) {
    uint32_t now = time_us();
    if (!clock.started) { clock.started = 1; clock.last = now; }
    clock.remainder += (uint32_t)(now - clock.last);
    clock.last = now;
    clock.ms += clock.remainder / 1000U;
    clock.remainder %= 1000U;
    return clock.ms;
}

static int link_poll(void* context) {
    (void)context;
    uint64_t now = now_ms();
    net_peer_poll(now);
    if (station.joining && now >= station.join_at) {
        station.joining = 0;
        station.result = net_peer_associate(station.ssid, station.ssid_bytes, station.passphrase,
                                            station.passphrase_bytes, station.hidden, &station.rssi);
        station.connected = station.result == 1;
        memset(station.passphrase, 0, sizeof(station.passphrase));
    }
    if (station.scanning && now >= station.scan_at) { station.scanning = 0; station.scanned = 1; }
    return 0;
}

static int link_send(void* context, const void* frame, unsigned int bytes) {
    (void)context;
    if (!station.connected) return NET_OFFLINE;
    net_peer_input(frame, bytes, now_ms());
    return 0;
}

static int link_receive(void* context, void* frame, unsigned int capacity) {
    (void)context;
    return station.connected ? (int)net_peer_output(frame, capacity, now_ms()) : 0;
}

static int link_join(void* context, const unsigned char* ssid, unsigned int ssid_bytes,
                     const unsigned char* passphrase, unsigned int passphrase_bytes, uint32_t flags) {
    (void)context;
    if (flags & ~NET_JOIN_HIDDEN) return NET_BAD;
    if (!ssid_bytes || ssid_bytes > NET_SSID_MAX || passphrase_bytes > 63) return NET_BAD;
    net_peer_disassociate();
    station.connected = station.result = 0;
    station.rssi = 0;
    memset(station.ssid, 0, sizeof(station.ssid));
    memset(station.passphrase, 0, sizeof(station.passphrase));
    memcpy(station.ssid, ssid, ssid_bytes);
    if (passphrase_bytes) memcpy(station.passphrase, passphrase, passphrase_bytes);
    station.ssid_bytes = ssid_bytes;
    station.passphrase_bytes = passphrase_bytes;
    station.hidden = (flags & NET_JOIN_HIDDEN) != 0;
    station.joining = 1;
    station.join_at = now_ms() + JOIN_MS;
    return 0;
}

static int link_join_status(void* context) {
    (void)context;
    if (station.joining) return NET_AGAIN;
    if (station.connected) return 1;
    return station.result < 0 ? station.result : NET_OFFLINE;
}

static int link_leave(void* context) {
    (void)context;
    net_peer_disassociate();
    station.joining = station.connected = 0;
    station.result = 0;
    memset(station.passphrase, 0, sizeof(station.passphrase));
    return 0;
}

static int link_scan(void* context) {
    (void)context;
    station.scanning = 1;
    station.scanned = 0;
    station.scan_at = now_ms() + SCAN_MS;
    return 0;
}

static int link_scan_status(void* context) {
    (void)context;
    return station.scanning ? NET_AGAIN : station.scanned ? 1 : 0;
}

static int link_scan_entry(void* context, unsigned int index, NetScan* entry) {
    (void)context;
    NetScan entries[NET_SCAN_LIMIT];
    unsigned int count = net_peer_scan(entries, NET_SCAN_LIMIT);
    if (index >= count) return 0;
    *entry = entries[index];
    return 1;
}

static int link_rssi(void* context, int32_t* dbm) {
    (void)context;
    if (!station.connected) return NET_OFFLINE;
    *dbm = station.rssi;
    return 0;
}

static void link_address(void* context, unsigned char address[6]) {
    (void)context;
    memcpy(address, mac, 6);
}

const NetLink* sim_net_link(void) {
    static const NetLink link = {
        .kind = NET_LINK_KIND_WIFI, .poll = link_poll, .send = link_send, .receive = link_receive,
        .address = link_address, .join = link_join, .join_status = link_join_status, .leave = link_leave,
        .scan = link_scan, .scan_status = link_scan_status, .scan_entry = link_scan_entry, .rssi = link_rssi,
    };
    return &link;
}

static int wired_poll(void* context) {
    (void)context;
    net_peer_poll(now_ms());
    return 0;
}

static int wired_send(void* context, const void* frame, unsigned int bytes) {
    (void)context;
    if (!cable) return NET_OFFLINE;
    net_peer_wired_input(frame, bytes, now_ms());
    return 0;
}

static int wired_receive(void* context, void* frame, unsigned int capacity) {
    (void)context;
    return cable ? (int)net_peer_wired_output(frame, capacity, now_ms()) : 0;
}

static void wired_address(void* context, unsigned char address[6]) {
    (void)context;
    memcpy(address, wired_mac, 6);
}

static int wired_carrier(void* context, uint32_t* mbps) {
    (void)context;
    if (!cable) return 0;
    *mbps = 1000;
    return 1;
}

const NetLink* sim_net_ethernet_link(void) {
    static const NetLink link = {
        .kind = NET_LINK_KIND_ETHERNET, .poll = wired_poll, .send = wired_send, .receive = wired_receive,
        .address = wired_address, .carrier = wired_carrier,
    };
    return &link;
}

void sim_net_set_cable(int up) {
    cable = up != 0;
    net_peer_set_cable(cable);
}

void sim_net_drop_link(void) {
    net_peer_disassociate();
    station.connected = station.joining = 0;
    station.result = NET_OFFLINE;
}

void sim_net_attach(void) {
    net_peer_reset(NULL);
    memset(&station, 0, sizeof(station));
    cable = 0;
    net_attach(sim_net_link(), sim_net_ethernet_link());
}
