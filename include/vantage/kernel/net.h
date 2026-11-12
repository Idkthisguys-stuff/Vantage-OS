#pragma once
#include <vantage/abi/net.h>

enum { NET_LINK_KIND_WIFI = 1, NET_LINK_KIND_ETHERNET = 2 };

/* A device exchanging Ethernet II frames: an 802.11 station or a wired MAC.
 * Every call is made by the network service on core 0 in masked kernel
 * context. Results are NetError values unless stated otherwise. */
typedef struct {
    void* context;
    uint32_t kind;
    /* Service the device once: 1 = progress made, 0 = idle, <0 = device lost. */
    int (*poll)(void* context);
    int (*send)(void* context, const void* frame, unsigned int bytes);
    /* Returns the frame size, 0 when no frame is queued, or an error. */
    int (*receive)(void* context, void* frame, unsigned int capacity);
    void (*address)(void* context, unsigned char mac[6]);

    /* Wi-Fi only; NULL for Ethernet. flags: NET_JOIN_HIDDEN. */
    int (*join)(void* context, const unsigned char* ssid, unsigned int ssid_bytes,
                const unsigned char* passphrase, unsigned int passphrase_bytes, uint32_t flags);
    /* 1 = connected, NET_AGAIN = joining, NET_OFFLINE = not associated, or the
     * join failure (NET_AUTH, NET_TIMEOUT, NET_IO, NET_NOT_FOUND). */
    int (*join_status)(void* context);
    int (*leave)(void* context);
    int (*scan)(void* context);
    /* 1 = complete, NET_AGAIN = running, 0 = no scan requested. */
    int (*scan_status)(void* context);
    /* Copies entry `index` into ssid/security/rssi/channel: 1, or 0 past the end. */
    int (*scan_entry)(void* context, unsigned int index, NetScan* entry);
    int (*rssi)(void* context, int32_t* dbm);

    /* Ethernet only; NULL for Wi-Fi. 1 with *mbps set while the cable is up,
     * 0 while it is down. */
    int (*carrier)(void* context, uint32_t* mbps);
} NetLink;

/* Core-0, masked, non-yielding service. Either link may be NULL; attached
 * links stay attached for kernel lifetime. Attaching resets the service. */
void net_attach(const NetLink* wifi, const NetLink* ethernet);
void net_poll(void);
int net_status(NetStatus* status);
int net_scan(NetScan* request);
int net_join(const NetJoin* request);
int net_leave(void);
int net_resolve(NetResolve* request);
int net_time(NetTime* time);
int net_profile(NetProfile* request);
/* Sockets belong to `owner` (a process or kernel task ID) and are released
 * when it is reaped. */
int64_t net_socket(uint64_t owner, NetSocketCall* call);
void net_release(uint64_t owner);
int64_t handle_net_call(uint64_t id, uintptr_t argument, uint64_t owner);

/* Platform entropy for transaction IDs, ports and TCP sequence numbers. */
uint32_t net_port_random(void);
/* Fills data from a hardware entropy source: 0, or NET_UNAVAILABLE without
 * one (data is then left unchanged). */
int net_entropy(void* data, unsigned int bytes);
