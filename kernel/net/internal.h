#pragma once
#include <vantage/kernel/net.h>

/* Private to kernel/net. Everything runs single-threaded on core 0 in masked
 * kernel context, from net_poll or a network syscall. */
#define ETH_HEADER 14U
#define ETH_FRAME_MAX 1514U
#define IP_HEADER 20U
#define IP_MTU 1500U
#define UDP_HEADER 8U
#define UDP_PAYLOAD_MAX (IP_MTU - IP_HEADER - UDP_HEADER)
#define TCP_HEADER 20U
#define TCP_MSS (IP_MTU - IP_HEADER - TCP_HEADER)
#define IP_BROADCAST 0xffffffffU

enum { IP_ICMP = 1, IP_TCP = 6, IP_UDP = 17 };

typedef struct {
    unsigned char mac[6];
    int bound;
    uint32_t address, netmask, gateway, dns[2], ntp[2];
    uint64_t rx_frames, tx_frames, rx_dropped;
} NetInterface;

/* net_interface describes the active link: Ethernet while its carrier is up,
 * otherwise associated Wi-Fi. */
extern NetInterface net_interface;

uint64_t net_now(void);              /* Milliseconds; monotonic, does not wrap. */
uint32_t net_random(void);
uint16_t net_port_ephemeral(void);   /* Unused local port, 49152..65535, or 0. */
int net_link_ready(void);            /* The active link can carry frames. */
int net_link_send(const void* frame, unsigned int bytes);
void net_address_bound(uint32_t previous);
void net_address_lost(void);         /* Address withdrawn: flush dependent state. */
int net_credentials_valid(uint32_t ssid_bytes, const char* passphrase, uint32_t passphrase_bytes);

/* Saved Wi-Fi profiles (profile.c), in insertion order. */
typedef struct {
    uint32_t flags;                  /* NET_PROFILE_HIDDEN */
    unsigned int ssid_bytes, passphrase_bytes;
    unsigned char ssid[NET_SSID_MAX], passphrase[64];
} NetSavedProfile;

unsigned int net_profile_count(void);
const NetSavedProfile* net_profile_at(unsigned int index);
void net_profile_reset(void);
int net_wifi_usable(void);
/* The profile table changed; `added` resumes paused automatic connection. */
void net_autojoin_changed(int added);

/* SNTP client (sntp.c); runs while the active interface has an address. */
void net_sntp_poll(void);
void net_sntp_reset(void);           /* Address lost: forget the time. */
int net_sntp_input(uint32_t source, uint16_t source_port, uint16_t port,
                   const unsigned char* data, unsigned int bytes);
int net_sntp_port_in_use(uint16_t port);

static inline uint16_t net_get16(const unsigned char* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t net_get32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void net_put16(unsigned char* p, uint32_t value) {
    p[0] = (unsigned char)(value >> 8); p[1] = (unsigned char)value;
}
static inline void net_put32(unsigned char* p, uint32_t value) {
    p[0] = (unsigned char)(value >> 24); p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8); p[3] = (unsigned char)value;
}
static inline int net_before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

/* Internet checksum over data, continuing from a partial sum. */
uint32_t net_sum(const void* data, unsigned int bytes, uint32_t sum);
uint16_t net_fold(uint32_t sum);
uint32_t net_pseudo(uint32_t source, uint32_t destination, unsigned int protocol, unsigned int bytes);

/* IPv4 output. Frames to unresolved neighbours wait for one ARP exchange;
 * returns 0 when sent or queued, or a NetError. source 0 uses our address. */
int net_ip_send(uint32_t source, uint32_t destination, unsigned int protocol,
                const void* payload, unsigned int bytes);
int net_udp_send(uint32_t source, uint32_t destination, uint16_t source_port,
                 uint16_t destination_port, const void* data, unsigned int bytes);
int net_icmp_echo(uint32_t destination, uint16_t identifier, uint16_t sequence,
                  const void* data, unsigned int bytes);
void net_frame_input(const unsigned char* frame, unsigned int bytes);
void net_ip_poll(void);
void net_ip_reset(void);             /* Address lost: flush neighbours. */

/* Upper layers. Inputs receive the transport header and payload. */
void net_dhcp_start(void);
void net_dhcp_stop(int release);
void net_dhcp_poll(void);
void net_dhcp_input(const unsigned char* data, unsigned int bytes);
uint32_t net_dhcp_state(void);       /* NET_ADDRESS_* */
uint32_t net_dhcp_lease_seconds(void);

void net_dns_reset(void);
void net_dns_poll(void);
int net_dns_input(uint32_t source, uint16_t source_port, uint16_t port,
                  const unsigned char* data, unsigned int bytes);
int net_dns_resolve(const char* name, uint32_t* address);
int net_dns_port_in_use(uint16_t port);
int net_parse_ipv4(const char* text, uint32_t* address);

/* Sockets. The table lives in socket.c; TCP state machines in tcp.c. */
#define DGRAM_SLOTS 4U
#define DGRAM_MAX UDP_PAYLOAD_MAX
#define TCP_TX_BYTES 4096U
#define TCP_RX_BYTES 8192U

enum { SOCKET_FREE, SOCKET_OPEN, SOCKET_ORPHAN };
enum {
    TCP_CLOSED, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_FIN_WAIT_1, TCP_FIN_WAIT_2,
    TCP_CLOSING, TCP_TIME_WAIT, TCP_CLOSE_WAIT, TCP_LAST_ACK
};

typedef struct {
    uint32_t address;
    uint16_t port, bytes;
    unsigned char data[DGRAM_MAX];
} NetDatagram;

typedef struct {
    int state, error;
    uint32_t iss, snd_una, snd_nxt, snd_max, snd_wnd, snd_wl1, snd_wl2, rcv_nxt;
    uint32_t mss, cwnd, ssthresh, rto, srtt, rttvar, rtt_seq, fin_seq, advertised;
    unsigned int retries, dupacks, tx_head, tx_used, rx_head, rx_used;
    int rtt_active, rtt_valid, fin_queued, fin_sent, fin_out, fin_received, probe;
    uint64_t timer, wait, linger, rtt_start;
    unsigned char tx[TCP_TX_BYTES], rx[TCP_RX_BYTES];
} NetTcp;

typedef struct {
    int state, error;
    uint32_t type, generation, remote;
    uint64_t owner;
    uint16_t local_port, remote_port, identifier;
    union {
        struct { NetDatagram queue[DGRAM_SLOTS]; unsigned int head, count, dropped; } dgram;
        NetTcp tcp;
    } u;
} NetSocket;

extern NetSocket net_sockets[NET_SOCKET_LIMIT];
void net_socket_free(NetSocket* socket);

int net_udp_deliver(uint32_t source, uint16_t source_port, uint16_t port,
                    const unsigned char* data, unsigned int bytes);
void net_icmp_deliver(uint32_t source, uint16_t identifier, uint16_t sequence,
                      const unsigned char* data, unsigned int bytes);
void net_icmp_unreachable(unsigned int code, const unsigned char* original, unsigned int bytes);
void net_socket_poll(void);
void net_socket_reset(void);         /* Address lost: TCP connections are reset. */
int net_socket_port_in_use(uint16_t port);

int net_tcp_connect(NetSocket* socket);
int net_tcp_write(NetSocket* socket, const unsigned char* data, unsigned int bytes);
int net_tcp_read(NetSocket* socket, unsigned char* data, unsigned int capacity);
void net_tcp_close(NetSocket* socket, int abort);
void net_tcp_timer(NetSocket* socket);
void net_tcp_fail(NetSocket* socket, int error);
int net_tcp_state(const NetSocket* socket);
void net_tcp_input(uint32_t source, uint32_t destination, const unsigned char* segment, unsigned int bytes);
