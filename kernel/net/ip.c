/* Ethernet II, ARP (RFC 826), IPv4 (RFC 791/1122), ICMP echo and errors
 * (RFC 792) and UDP (RFC 768). No fragmentation, options or multicast. */
#include "internal.h"

#define ARP_ENTRIES 8U
#define ARP_RETRY_MS 1000U
#define ARP_TRIES 3U
#define ARP_LIFETIME_MS 300000U
#define ARP_FAILED_MS 3000U

enum { ARP_FREE, ARP_PENDING, ARP_RESOLVED, ARP_FAILED };

typedef struct {
    uint32_t address;
    unsigned char mac[6];
    int state;
    unsigned int tries, queued;
    uint64_t deadline, used;
    unsigned char frame[ETH_FRAME_MAX];
} ArpEntry;

NetInterface net_interface;
static ArpEntry neighbours[ARP_ENTRIES];
static unsigned char output[ETH_FRAME_MAX];
static uint16_t ip_identifier;

uint32_t net_sum(const void* data, unsigned int bytes, uint32_t sum) {
    const unsigned char* p = data;
    for (; bytes > 1; bytes -= 2, p += 2) sum += (uint32_t)((p[0] << 8) | p[1]);
    if (bytes) sum += (uint32_t)p[0] << 8;
    return sum;
}

uint16_t net_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
    return (uint16_t)~sum;
}

uint32_t net_pseudo(uint32_t source, uint32_t destination, unsigned int protocol, unsigned int bytes) {
    return (source >> 16) + (source & 0xffffU) + (destination >> 16) + (destination & 0xffffU) +
           protocol + bytes;
}

static int frame_send(const unsigned char* frame, unsigned int bytes) {
    int result = net_link_send(frame, bytes);
    if (result >= 0) net_interface.tx_frames++;
    return result;
}

static void arp_send(unsigned int operation, const unsigned char* mac, uint32_t target) {
    unsigned char frame[60] = {0};
    if (operation == 1) memset(frame, 0xff, 6); else memcpy(frame, mac, 6);
    memcpy(frame + 6, net_interface.mac, 6);
    net_put16(frame + 12, 0x0806);
    unsigned char* a = frame + ETH_HEADER;
    net_put16(a, 1); net_put16(a + 2, 0x0800); a[4] = 6; a[5] = 4; net_put16(a + 6, operation);
    memcpy(a + 8, net_interface.mac, 6); net_put32(a + 14, net_interface.address);
    if (operation == 2) memcpy(a + 18, mac, 6);
    net_put32(a + 24, target);
    (void)frame_send(frame, sizeof(frame));
}

static ArpEntry* neighbour(uint32_t address) {
    for (unsigned int i = 0; i < ARP_ENTRIES; i++)
        if (neighbours[i].state != ARP_FREE && neighbours[i].address == address) return &neighbours[i];
    return NULL;
}

static ArpEntry* neighbour_new(uint32_t address) {
    ArpEntry* victim = NULL;
    for (unsigned int i = 0; i < ARP_ENTRIES; i++) {
        ArpEntry* e = &neighbours[i];
        if (e->state == ARP_FREE) { victim = e; break; }
        if (e->state != ARP_PENDING && (!victim || e->used < victim->used)) victim = e;
    }
    if (!victim) return NULL;
    memset(victim, 0, sizeof(*victim) - sizeof(victim->frame));
    victim->address = address;
    return victim;
}

static void neighbour_learn(ArpEntry* e, const unsigned char* mac) {
    memcpy(e->mac, mac, 6);
    e->state = ARP_RESOLVED;
    e->deadline = net_now() + ARP_LIFETIME_MS;
    if (e->queued) {
        memcpy(e->frame, mac, 6);
        (void)frame_send(e->frame, e->queued);
        e->queued = 0;
    }
}

static void arp_input(const unsigned char* a, unsigned int bytes) {
    if (bytes < 28 || net_get16(a) != 1 || net_get16(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4) return;
    unsigned int operation = net_get16(a + 6);
    uint32_t sender = net_get32(a + 14), target = net_get32(a + 24);
    if ((operation != 1 && operation != 2) || (a[8] & 1)) return;
    if (!net_interface.bound || !sender || sender == net_interface.address) return;
    ArpEntry* e = neighbour(sender);
    if (!e && operation == 1 && target == net_interface.address) e = neighbour_new(sender);
    if (e) neighbour_learn(e, a + 8);
    if (operation == 1 && target == net_interface.address) arp_send(2, a + 8, sender);
}

void net_ip_reset(void) {
    for (unsigned int i = 0; i < ARP_ENTRIES; i++) neighbours[i].state = ARP_FREE, neighbours[i].queued = 0;
}

void net_ip_poll(void) {
    uint64_t now = net_now();
    for (unsigned int i = 0; i < ARP_ENTRIES; i++) {
        ArpEntry* e = &neighbours[i];
        if (e->state == ARP_PENDING && now >= e->deadline) {
            if (e->tries < ARP_TRIES && net_interface.bound) {
                e->tries++;
                e->deadline = now + ARP_RETRY_MS;
                arp_send(1, NULL, e->address);
            } else {
                e->state = ARP_FAILED; e->queued = 0;
                e->deadline = now + ARP_FAILED_MS;
            }
        } else if ((e->state == ARP_FAILED || e->state == ARP_RESOLVED) && now >= e->deadline + ARP_LIFETIME_MS) {
            e->state = ARP_FREE;
        }
    }
}

static int is_broadcast(uint32_t address) {
    return address == IP_BROADCAST ||
           (net_interface.bound && net_interface.netmask != IP_BROADCAST &&
            address == (net_interface.address | ~net_interface.netmask));
}

int net_ip_send(uint32_t source, uint32_t destination, unsigned int protocol,
                const void* payload, unsigned int bytes) {
    if (!net_link_ready()) return NET_OFFLINE;
    if (bytes > IP_MTU - IP_HEADER) return NET_BAD;
    int broadcast = is_broadcast(destination);
    if (!broadcast && (!net_interface.bound || !destination)) return NET_OFFLINE;
    if ((destination >> 28) == 14 || (destination >> 24) == 127 || (destination >> 24) == 0) return NET_UNREACHABLE;
    unsigned char* ip = output + ETH_HEADER;
    unsigned int total = IP_HEADER + bytes;
    memcpy(output + 6, net_interface.mac, 6);
    net_put16(output + 12, 0x0800);
    ip[0] = 0x45; ip[1] = 0; net_put16(ip + 2, total); net_put16(ip + 4, ip_identifier++);
    net_put16(ip + 6, 0x4000); ip[8] = 64; ip[9] = (unsigned char)protocol; net_put16(ip + 10, 0);
    net_put32(ip + 12, source); net_put32(ip + 16, destination);
    net_put16(ip + 10, net_fold(net_sum(ip, IP_HEADER, 0)));
    memcpy(ip + IP_HEADER, payload, bytes);
    total += ETH_HEADER;
    if (broadcast) {
        memset(output, 0xff, 6);
        return frame_send(output, total) < 0 ? NET_IO : 0;
    }
    uint32_t hop = destination;
    if ((destination & net_interface.netmask) != (net_interface.address & net_interface.netmask)) {
        if (!net_interface.gateway) return NET_UNREACHABLE;
        hop = net_interface.gateway;
    }
    uint64_t now = net_now();
    ArpEntry* e = neighbour(hop);
    if (e && e->state == ARP_FAILED && now < e->deadline) return NET_UNREACHABLE;
    if (e && e->state == ARP_RESOLVED && now < e->deadline) {
        e->used = now;
        memcpy(output, e->mac, 6);
        return frame_send(output, total) < 0 ? NET_IO : 0;
    }
    if (!e || e->state != ARP_PENDING) {
        if (!e) e = neighbour_new(hop);
        if (!e) return NET_AGAIN;
        e->state = ARP_PENDING; e->tries = 1; e->deadline = now + ARP_RETRY_MS;
        arp_send(1, NULL, hop);
    }
    /* Keep the newest frame for this neighbour; transports retransmit. */
    e->used = now;
    memcpy(e->frame, output, total);
    e->queued = total;
    return 0;
}

int net_udp_send(uint32_t source, uint32_t destination, uint16_t source_port,
                 uint16_t destination_port, const void* data, unsigned int bytes) {
    unsigned char datagram[IP_MTU - IP_HEADER];
    if (bytes > UDP_PAYLOAD_MAX || !source_port || !destination_port) return NET_BAD;
    unsigned int length = UDP_HEADER + bytes;
    net_put16(datagram, source_port); net_put16(datagram + 2, destination_port);
    net_put16(datagram + 4, length); net_put16(datagram + 6, 0);
    if (bytes) memcpy(datagram + UDP_HEADER, data, bytes);
    uint16_t sum = net_fold(net_sum(datagram, length, net_pseudo(source, destination, IP_UDP, length)));
    net_put16(datagram + 6, sum ? sum : 0xffffU);
    return net_ip_send(source, destination, IP_UDP, datagram, length);
}

static int icmp_send(uint32_t destination, unsigned int type, unsigned int code, uint32_t rest,
                     const void* data, unsigned int bytes) {
    unsigned char message[IP_MTU - IP_HEADER];
    if (bytes > sizeof(message) - 8) return NET_BAD;
    message[0] = (unsigned char)type; message[1] = (unsigned char)code; net_put16(message + 2, 0);
    net_put32(message + 4, rest);
    if (bytes) memcpy(message + 8, data, bytes);
    net_put16(message + 2, net_fold(net_sum(message, 8 + bytes, 0)));
    return net_ip_send(net_interface.address, destination, IP_ICMP, message, 8 + bytes);
}

int net_icmp_echo(uint32_t destination, uint16_t identifier, uint16_t sequence,
                  const void* data, unsigned int bytes) {
    return icmp_send(destination, 8, 0, ((uint32_t)identifier << 16) | sequence, data, bytes);
}

static void icmp_input(uint32_t source, int unicast, const unsigned char* m, unsigned int bytes) {
    if (bytes < 8 || net_fold(net_sum(m, bytes, 0))) return;
    if (m[0] == 8 && !m[1] && unicast) {
        (void)icmp_send(source, 0, 0, net_get32(m + 4), m + 8, bytes - 8);
    } else if (m[0] == 0 && !m[1]) {
        net_icmp_deliver(source, net_get16(m + 4), net_get16(m + 6), m + 8, bytes - 8);
    } else if (m[0] == 3) {
        net_icmp_unreachable(m[1], m + 8, bytes - 8);
    }
}

static void udp_input(uint32_t source, uint32_t destination, int unicast,
                      const unsigned char* u, unsigned int bytes) {
    if (bytes < UDP_HEADER) return;
    unsigned int length = net_get16(u + 4);
    if (length < UDP_HEADER || length > bytes) return;
    if (net_get16(u + 6) && net_fold(net_sum(u, length, net_pseudo(source, destination, IP_UDP, length)))) return;
    uint16_t source_port = net_get16(u), port = net_get16(u + 2);
    const unsigned char* data = u + UDP_HEADER;
    unsigned int data_bytes = length - UDP_HEADER;
    if (port == 68 && source_port == 67) { net_dhcp_input(data, data_bytes); return; }
    if (!net_interface.bound) return;
    if (net_dns_input(source, source_port, port, data, data_bytes)) return;
    if (net_sntp_input(source, source_port, port, data, data_bytes)) return;
    if (net_udp_deliver(source, source_port, port, data, data_bytes)) return;
    /* RFC 1122: report closed ports for unicast datagrams only. */
    if (unicast) {
        const unsigned char* ip = u - IP_HEADER;
        unsigned char original[IP_HEADER + 8];
        memcpy(original, ip, IP_HEADER);
        memcpy(original + IP_HEADER, u, 8);
        (void)icmp_send(source, 3, 3, 0, original, sizeof(original));
    }
}

static void ip_input(const unsigned char* ip, unsigned int bytes) {
    if (bytes < IP_HEADER || (ip[0] >> 4) != 4) return;
    unsigned int header = (ip[0] & 15U) * 4U, total = net_get16(ip + 2);
    if (header < IP_HEADER || header > bytes || total < header || total > bytes) return;
    if (net_fold(net_sum(ip, header, 0)) || (net_get16(ip + 6) & 0x3fffU)) return;
    uint32_t source = net_get32(ip + 12), destination = net_get32(ip + 16);
    if (source == IP_BROADCAST || (source >> 28) == 14 || (source >> 24) == 127) return;
    int unicast = net_interface.bound && destination == net_interface.address;
    int broadcast = is_broadcast(destination);
    unsigned int protocol = ip[9];
    if (!unicast && !broadcast && (protocol != IP_UDP || net_interface.bound)) return;
    /* Before an address is bound only DHCP replies (UDP 67 -> 68) are taken. */
    if (header != IP_HEADER) {
        /* Options are ignored; keep the transport aligned on a plain header. */
        static unsigned char copy[IP_MTU];
        memcpy(copy, ip, IP_HEADER);
        memcpy(copy + IP_HEADER, ip + header, total - header);
        copy[0] = 0x45;
        net_put16(copy + 2, total - header + IP_HEADER);
        ip = copy; total = total - header + IP_HEADER;
    }
    const unsigned char* payload = ip + IP_HEADER;
    unsigned int length = total - IP_HEADER;
    if (protocol == IP_ICMP && net_interface.bound) icmp_input(source, unicast, payload, length);
    else if (protocol == IP_UDP) udp_input(source, destination, unicast, payload, length);
    else if (protocol == IP_TCP && unicast) net_tcp_input(source, destination, payload, length);
}

void net_frame_input(const unsigned char* frame, unsigned int bytes) {
    if (bytes < ETH_HEADER) return;
    int ours = !memcmp(frame, net_interface.mac, 6);
    int broadcast = frame[0] == 0xff && frame[1] == 0xff && frame[2] == 0xff &&
                    frame[3] == 0xff && frame[4] == 0xff && frame[5] == 0xff;
    if (!ours && !broadcast) { net_interface.rx_dropped++; return; }
    net_interface.rx_frames++;
    unsigned int type = net_get16(frame + 12);
    if (type == 0x0806) arp_input(frame + ETH_HEADER, bytes - ETH_HEADER);
    else if (type == 0x0800) ip_input(frame + ETH_HEADER, bytes - ETH_HEADER);
}
