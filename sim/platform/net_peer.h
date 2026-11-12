#pragma once
#include <vantage/abi/net.h>

/* A deterministic virtual network for the simulator and host tests: four
 * access points (one hidden), a wired switch port, a router (192.168.50.1:
 * ARP, DHCP, DNS, NTP, ICMP), a web host (192.168.50.10, also NTP) and an
 * off-link host (203.0.113.10) behind the router. Each station MAC leases
 * its own address, 192.168.50.100 upwards in order of first request. Web
 * hosts serve TCP 80 (HTTP), 443 (HTTPS, tls_peer.h), 7 (echo), 9 (discard)
 * and 19 (chargen). Faults are injected per frame. It is test
 * infrastructure, not a router. */
#define NET_PEER_ROUTER 0xc0a83201U
#define NET_PEER_WEB 0xc0a8320aU
#define NET_PEER_REMOTE 0xcb00710aU
#define NET_PEER_CLIENT 0xc0a83264U
#define NET_PEER_PASSPHRASE "vantage-demo"
#define NET_PEER_HIDDEN_PASSPHRASE "hidden-demo"
#define NET_PEER_UNIX_EPOCH 1794398400ULL  /* NTP time at peer time 0: 2026-11-11 12:00:00 UTC. */

/* Access points: VantageNet (WPA2), Vantage Guest (open), Neighbour WPA3, and
 * Vantage Hidden (WPA2, never in scans, joined only by direct probe). */
enum { NET_PEER_AP_MAIN = 1, NET_PEER_AP_GUEST = 2, NET_PEER_AP_WPA3 = 4, NET_PEER_AP_HIDDEN = 8 };
/* Faulty router NTP replies: no reply, kiss-o'-death, server mode 5,
 * version 2, wrong originate, leap 3, stratum 16, zero transmit time, from
 * port 1123, or from the web host's address. */
enum {
    NET_PEER_NTP_GOOD, NET_PEER_NTP_SILENT, NET_PEER_NTP_KOD, NET_PEER_NTP_MODE, NET_PEER_NTP_VERSION,
    NET_PEER_NTP_ORIGIN, NET_PEER_NTP_UNSYNCED, NET_PEER_NTP_STRATUM, NET_PEER_NTP_ZERO, NET_PEER_NTP_PORT,
    NET_PEER_NTP_SOURCE
};

typedef struct {
    uint32_t lease_seconds;       /* 0 selects 3600. */
    uint32_t latency_ms;          /* Delay of every frame towards the station. */
    uint32_t drop_up, drop_down;  /* Drop every Nth frame in that direction. */
    uint32_t duplicate_down, reorder_down;
    uint32_t server_window;       /* TCP window the web hosts advertise; 0 = 65535. */
    uint32_t chargen_bytes;       /* Stream length on port 19; 0 = 100000. */
    uint32_t close_window_ms;     /* Port 9 advertises a zero window for this long. */
    int nak_renewal, dns_silent, dhcp_silent, server_silent;
    uint32_t aps_off;             /* NET_PEER_AP_* switched off: not scanned, not joinable. */
    uint32_t ntp_fault;           /* NET_PEER_NTP_* for the router's replies. */
    int ntp_genuine;              /* Follow a faulty reply with the correct one. */
    int no_ntp_option, ntp_web;   /* DHCP option 42: omitted, or router and web host. */
    uint64_t ntp_epoch;           /* Unix seconds at peer time 0; 0 selects NET_PEER_UNIX_EPOCH. */
} NetPeerOptions;

typedef struct {
    uint64_t up, down, dropped, dhcp_acks, dhcp_naks, dhcp_releases, dns_queries;
    uint64_t echo_replies, arp_replies, tcp_retransmits, tcp_resets, discarded, segments;
    uint64_t probes, invalid, http_requests, echoed, ntp_queries, joins;
} NetPeerStats;

void net_peer_reset(const NetPeerOptions* options);
NetPeerOptions* net_peer_options(void);
const NetPeerStats* net_peer_stats(void);
/* A frame from the Wi-Fi station; responses are queued for net_peer_output. */
void net_peer_input(const unsigned char* frame, unsigned int bytes, uint64_t now_ms);
/* Next frame due for the Wi-Fi station: its size, or 0. */
unsigned int net_peer_output(unsigned char* frame, unsigned int capacity, uint64_t now_ms);
void net_peer_poll(uint64_t now_ms);
/* The wired port: frames pass only while the cable is plugged in. */
void net_peer_set_cable(int up);
void net_peer_wired_input(const unsigned char* frame, unsigned int bytes, uint64_t now_ms);
unsigned int net_peer_wired_output(unsigned char* frame, unsigned int capacity, uint64_t now_ms);
/* The NTP servers' current time in Unix milliseconds. */
uint64_t net_peer_unix_ms(void);

unsigned int net_peer_scan(NetScan* entries, unsigned int capacity);
/* 1 = associated, or NET_AUTH / NET_NOT_FOUND. `hidden` probes by name. */
int net_peer_associate(const unsigned char* ssid, unsigned int ssid_bytes, const unsigned char* passphrase,
                       unsigned int passphrase_bytes, int hidden, int32_t* rssi);
void net_peer_disassociate(void);
