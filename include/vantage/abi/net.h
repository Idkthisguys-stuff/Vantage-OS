#pragma once
#include <vantage/base.h>

/* Wi-Fi and Ethernet link control, IPv4 configuration, name resolution,
 * sockets, network time and entropy. IPv4 addresses are host integers with
 * the first dotted octet in the most significant byte: 192.168.1.2 is
 * 0xc0a80102. Ports are host integers. */
#define NET_SSID_MAX 32U
#define NET_PASSPHRASE_MIN 8U
#define NET_PASSPHRASE_MAX 63U
#define NET_NAME_MAX 256U
#define NET_IO_MAX 1024U
#define NET_SCAN_LIMIT 16U
#define NET_SOCKET_LIMIT 8U
#define NET_PROFILE_LIMIT 8U
#define NET_RANDOM_MAX 256U

enum NetError {
    NET_BAD = -1, NET_UNAVAILABLE = -2, NET_OFFLINE = -3, NET_AGAIN = -4,
    NET_TIMEOUT = -5, NET_NO_SPACE = -6, NET_BAD_HANDLE = -7, NET_REFUSED = -8,
    NET_RESET = -9, NET_NOT_FOUND = -10, NET_AUTH = -11, NET_IO = -12,
    NET_BUSY = -13, NET_UNREACHABLE = -14, NET_UNSUPPORTED = -15
};

enum { NET_LINK_DOWN, NET_LINK_JOINING, NET_LINK_UP, NET_LINK_RETRYING };
enum { NET_ADDRESS_NONE, NET_ADDRESS_CONFIGURING, NET_ADDRESS_BOUND };
enum {
    NET_SECURITY_OPEN, NET_SECURITY_WEP, NET_SECURITY_WPA, NET_SECURITY_WPA2,
    NET_SECURITY_WPA3, NET_SECURITY_WPA2_WPA3, NET_SECURITY_ENTERPRISE, NET_SECURITY_OTHER
};

#define SYS_NET_STATUS 48U
#define SYS_NET_SCAN 49U
#define SYS_NET_JOIN 50U
#define SYS_NET_LEAVE 51U
#define SYS_NET_RESOLVE 52U
#define SYS_NET_SOCKET 53U
#define SYS_NET_TIME 54U
#define SYS_NET_PROFILE 55U
#define SYS_NET_RANDOM 56U

enum { NET_INTERFACE_NONE, NET_INTERFACE_WIFI, NET_INTERFACE_ETHERNET };
enum { NET_ETHERNET_ABSENT, NET_ETHERNET_NO_CABLE, NET_ETHERNET_UP };
#define NET_STATUS_HIDDEN 1U
#define NET_STATUS_PROFILE 2U

/* STATUS: available, link, security, ssid, rssi, error and retries describe
 * Wi-Fi; available is 0 without a usable radio. error is the last join/link
 * failure, or 0. interface carries IPv4 traffic: Ethernet while its cable is
 * up, otherwise Wi-Fi. ethernet_mbps is the negotiated speed while up. flags:
 * NET_STATUS_HIDDEN for a hidden network, NET_STATUS_PROFILE when the network
 * was joined automatically from a saved profile. lease_seconds is the
 * remaining DHCP lease (0xffffffff for an infinite lease). */
typedef struct {
    uint32_t available, link, security, ssid_length;
    char ssid[NET_SSID_MAX + 4];
    int32_t rssi, error;
    uint32_t address, netmask, gateway, dns;
    uint32_t configuration, lease_seconds, retries;
    uint32_t interface, ethernet, ethernet_mbps, flags, reserved[2];
    uint64_t rx_frames, tx_frames, rx_dropped;
} NetStatus;

/* SCAN START begins a new scan. RESULT copies entry `cursor` and advances it:
 * 1 = entry, 0 = end of the completed scan, NET_AGAIN = still scanning. */
enum { NET_SCAN_START, NET_SCAN_RESULT };
typedef struct {
    uint32_t operation, cursor;
    int32_t rssi;
    uint32_t channel, security, ssid_length;
    char ssid[NET_SSID_MAX + 4];
    uint32_t reserved;
} NetScan;

/* JOIN starts association and returns 0; progress is reported by STATUS.
 * passphrase_length 0 selects an open network explicitly; otherwise the
 * passphrase is 8..63 printable ASCII characters (WPA2-PSK). NET_JOIN_HIDDEN
 * probes for an SSID that is not broadcast. An explicit join takes over from
 * automatic connection; LEAVE pauses automatic connection until the next
 * JOIN or profile ADD. */
#define NET_JOIN_HIDDEN 1U
typedef struct {
    uint32_t ssid_length, passphrase_length;
    char ssid[NET_SSID_MAX];
    char passphrase[64];
    uint32_t flags, reserved;
} NetJoin;

/* Saved networks the kernel joins automatically while Wi-Fi is not connected
 * and not paused: the strongest visible profile first, hidden profiles by
 * direct probe. Profiles live in kernel memory only; programs persist them.
 * ADD replaces a profile with the same SSID (0, or NET_NO_SPACE). REMOVE
 * matches ssid (0, or NET_NOT_FOUND). CLEAR removes all. LIST copies profile
 * `cursor` and advances it: 1 = entry, 0 = end; the passphrase is never
 * returned and passphrase_length reports only whether one is set (0 or 1).
 * flags: NET_PROFILE_HIDDEN. */
enum { NET_PROFILE_ADD, NET_PROFILE_REMOVE, NET_PROFILE_LIST, NET_PROFILE_CLEAR };
#define NET_PROFILE_HIDDEN 1U
typedef struct {
    uint32_t operation, cursor, flags, ssid_length, passphrase_length;
    char ssid[NET_SSID_MAX];
    char passphrase[64];
    uint32_t reserved;
} NetProfile;

/* TIME returns 1 once SNTP has set the clock, otherwise 0 with the time fields
 * zero. unix_seconds/microseconds are the current UTC time, server answered
 * the last accepted query and age_seconds counts since then. */
typedef struct {
    uint32_t synced, server;
    uint64_t unix_seconds;
    uint32_t microseconds, age_seconds;
} NetTime;

/* RANDOM fills data[0..length) with bytes from a hardware entropy source
 * (length 1..NET_RANDOM_MAX, flags 0) and returns length, or NET_UNAVAILABLE
 * when only a weak source exists; nothing is written then. */
typedef struct {
    uint32_t length, flags;
    unsigned char data[NET_RANDOM_MAX];
} NetRandom;

/* RESOLVE returns 1 with `address`, NET_AGAIN while the query is pending,
 * or an error. Dotted IPv4 literals resolve immediately. flags must be 0. */
typedef struct {
    uint32_t flags, address;
    char name[NET_NAME_MAX];
} NetResolve;

enum { NET_UDP = 1, NET_TCP = 2, NET_ICMP = 3 };
enum {
    NET_SOCKET_OPEN, NET_SOCKET_CONNECT, NET_SOCKET_SEND,
    NET_SOCKET_RECEIVE, NET_SOCKET_CLOSE, NET_SOCKET_STATUS
};
enum {
    NET_TCP_CLOSED, NET_TCP_CONNECTING, NET_TCP_ESTABLISHED,
    NET_TCP_CLOSING, NET_TCP_PEER_CLOSED
};
#define NET_CLOSE_ABORT 1U

/* One socket operation. OPEN takes `type` and returns a handle. CONNECT starts
 * a TCP connection (NET_AGAIN until established) or sets the UDP/ICMP peer.
 * SEND returns bytes accepted; UDP uses address/port when address is nonzero.
 * ICMP sends an echo request whose sequence is `port`. RECEIVE returns bytes,
 * with the source address/port (ICMP: the echo sequence); 0 is TCP end of
 * stream. STATUS returns `state`; `pending` is the readable byte count. */
typedef struct {
    uint32_t operation, type;
    uint64_t handle;
    uint32_t address;
    uint16_t port, local_port;
    uint32_t length, flags;
    int32_t state;
    uint32_t pending;
    unsigned char data[NET_IO_MAX];
} NetSocketCall;
