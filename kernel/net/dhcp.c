/* DHCPv4 client (RFC 2131/2132): discover, request, renew, rebind, release. */
#include "internal.h"

#define BOOTP_BYTES 300U
#define COOKIE 0x63825363U
#define FOREVER (~(uint64_t)0)

enum { DHCP_OFF, DHCP_SELECTING, DHCP_REQUESTING, DHCP_BOUND, DHCP_RENEWING, DHCP_REBINDING };
enum { DISCOVER = 1, OFFER = 2, REQUEST = 3, ACK = 5, NAK = 6, RELEASE = 7 };

static struct {
    int state;
    uint32_t xid, offered, server;
    unsigned int attempts;
    uint64_t next, started, t1, t2, expiry;
} dhcp;

static int usable(uint32_t address) {
    unsigned int top = address >> 24;
    return address && address != IP_BROADCAST && top != 0 && top != 127 && top < 224;
}

static int send_message(unsigned int type) {
    unsigned char m[BOOTP_BYTES] = {0};
    int renewing = dhcp.state == DHCP_RENEWING || dhcp.state == DHCP_REBINDING || type == RELEASE;
    m[0] = 1; m[1] = 1; m[2] = 6;
    net_put32(m + 4, dhcp.xid);
    uint64_t seconds = (net_now() - dhcp.started) / 1000U;
    net_put16(m + 8, seconds > 0xffffU ? 0xffffU : (uint32_t)seconds);
    if (renewing) net_put32(m + 12, net_interface.address);
    memcpy(m + 28, net_interface.mac, 6);
    net_put32(m + 236, COOKIE);
    unsigned char* o = m + 240;
    *o++ = 53; *o++ = 1; *o++ = (unsigned char)type;
    *o++ = 61; *o++ = 7; *o++ = 1; memcpy(o, net_interface.mac, 6); o += 6;
    if (type == REQUEST && dhcp.state == DHCP_REQUESTING) {
        *o++ = 50; *o++ = 4; net_put32(o, dhcp.offered); o += 4;
        *o++ = 54; *o++ = 4; net_put32(o, dhcp.server); o += 4;
    } else if (type == RELEASE) {
        *o++ = 54; *o++ = 4; net_put32(o, dhcp.server); o += 4;
    }
    if (type != RELEASE) {
        static const unsigned char wanted[] = {55, 7, 1, 3, 6, 42, 51, 58, 59};
        memcpy(o, wanted, sizeof(wanted)); o += sizeof(wanted);
        *o++ = 57; *o++ = 2; net_put16(o, 1500); o += 2;
        *o++ = 12; *o++ = 7; memcpy(o, "vantage", 7); o += 7;
    }
    *o = 255;
    if (dhcp.state == DHCP_RENEWING || type == RELEASE)
        return net_udp_send(net_interface.address, dhcp.server, 68, 67, m, sizeof(m));
    return net_udp_send(renewing ? net_interface.address : 0, IP_BROADCAST, 68, 67, m, sizeof(m));
}

static uint64_t later(uint64_t now, uint64_t delay) {
    return delay == FOREVER || now > FOREVER - delay ? FOREVER : now + delay;
}

static void select_again(void) {
    dhcp.state = DHCP_SELECTING;
    dhcp.xid = net_random();
    dhcp.attempts = 0;
    dhcp.started = net_now();
    dhcp.next = dhcp.started + net_random() % 500U;
}

void net_dhcp_start(void) { select_again(); }

void net_dhcp_stop(int release) {
    if (release && net_interface.bound && dhcp.server) (void)send_message(RELEASE);
    memset(&dhcp, 0, sizeof(dhcp));
}

uint32_t net_dhcp_state(void) {
    if (dhcp.state == DHCP_OFF) return NET_ADDRESS_NONE;
    return dhcp.state >= DHCP_BOUND ? NET_ADDRESS_BOUND : NET_ADDRESS_CONFIGURING;
}

uint32_t net_dhcp_lease_seconds(void) {
    if (dhcp.state < DHCP_BOUND) return 0;
    if (dhcp.expiry == FOREVER) return 0xffffffffU;
    uint64_t now = net_now();
    return now >= dhcp.expiry ? 0 : (uint32_t)((dhcp.expiry - now) / 1000U);
}

static void lost(void) {
    if (net_interface.bound) net_address_lost();
    select_again();
}

void net_dhcp_poll(void) {
    uint64_t now = net_now();
    if (dhcp.state == DHCP_OFF) return;
    if (dhcp.state >= DHCP_BOUND && now >= dhcp.expiry) { lost(); return; }
    if (dhcp.state == DHCP_BOUND && now >= dhcp.t1) { dhcp.state = DHCP_RENEWING; dhcp.next = now; }
    if (dhcp.state == DHCP_RENEWING && now >= dhcp.t2) { dhcp.state = DHCP_REBINDING; dhcp.next = now; }
    if (dhcp.state == DHCP_BOUND || now < dhcp.next) return;
    if (dhcp.state == DHCP_SELECTING) {
        (void)send_message(DISCOVER);
        unsigned int shift = dhcp.attempts < 4 ? dhcp.attempts : 4;
        dhcp.next = now + (4000ULL << shift) - 1000U + net_random() % 2000U;
        dhcp.attempts++;
    } else if (dhcp.state == DHCP_REQUESTING) {
        if (dhcp.attempts >= 4) { select_again(); return; }
        (void)send_message(REQUEST);
        dhcp.next = now + (4000ULL << dhcp.attempts);
        dhcp.attempts++;
    } else {
        /* RFC 2131 4.4.5: retry at half the remaining time, at least 60 s. */
        (void)send_message(REQUEST);
        uint64_t end = dhcp.state == DHCP_RENEWING ? dhcp.t2 : dhcp.expiry;
        uint64_t wait = end == FOREVER ? 60000U : (end - now) / 2U;
        dhcp.next = later(now, wait < 60000U ? 60000U : wait);
    }
}

typedef struct {
    unsigned int type;
    uint32_t netmask, router, server, dns[2], ntp[2], lease, t1, t2;
    int has_netmask, has_lease, has_t1, has_t2;
} Options;

static int parse(const unsigned char* o, unsigned int bytes, Options* out) {
    memset(out, 0, sizeof(*out));
    for (unsigned int i = 0; i < bytes;) {
        unsigned int code = o[i++];
        if (code == 0) continue;
        if (code == 255) return out->type != 0;
        if (i >= bytes) return 0;
        unsigned int length = o[i++];
        if (length > bytes - i) return 0;
        const unsigned char* v = o + i;
        if (code == 53 && length == 1) out->type = v[0];
        else if (code == 1 && length == 4) { out->netmask = net_get32(v); out->has_netmask = 1; }
        else if (code == 3 && length >= 4) out->router = net_get32(v);
        else if (code == 6 && length >= 4) {
            out->dns[0] = net_get32(v);
            if (length >= 8) out->dns[1] = net_get32(v + 4);
        } else if (code == 42 && length >= 4 && !(length % 4)) {
            out->ntp[0] = net_get32(v);
            if (length >= 8) out->ntp[1] = net_get32(v + 4);
        } else if (code == 51 && length == 4) { out->lease = net_get32(v); out->has_lease = 1; }
        else if (code == 54 && length == 4) out->server = net_get32(v);
        else if (code == 58 && length == 4) { out->t1 = net_get32(v); out->has_t1 = 1; }
        else if (code == 59 && length == 4) { out->t2 = net_get32(v); out->has_t2 = 1; }
        i += length;
    }
    return 0;
}

static void bind_lease(uint32_t address, const Options* o) {
    uint32_t mask = o->has_netmask ? o->netmask : 0xffffff00U;
    if (!mask || (~mask & (~mask + 1U))) return;  /* Non-contiguous mask. */
    uint32_t previous = net_interface.bound ? net_interface.address : 0;
    net_interface.address = address;
    net_interface.netmask = mask;
    net_interface.gateway = 0;
    if (usable(o->router) && o->router != address &&
        (mask == IP_BROADCAST || (o->router & mask) == (address & mask))) net_interface.gateway = o->router;
    net_interface.dns[0] = usable(o->dns[0]) ? o->dns[0] : 0;
    net_interface.dns[1] = usable(o->dns[1]) ? o->dns[1] : 0;
    if (!net_interface.dns[0]) { net_interface.dns[0] = net_interface.dns[1]; net_interface.dns[1] = 0; }
    net_interface.ntp[0] = usable(o->ntp[0]) ? o->ntp[0] : 0;
    net_interface.ntp[1] = usable(o->ntp[1]) ? o->ntp[1] : 0;
    if (!net_interface.ntp[0]) { net_interface.ntp[0] = net_interface.ntp[1]; net_interface.ntp[1] = 0; }
    net_interface.bound = 1;
    net_address_bound(previous);
    uint64_t now = net_now();
    if (o->lease == 0xffffffffU) {
        dhcp.t1 = dhcp.t2 = dhcp.expiry = FOREVER;
    } else {
        uint64_t lease = (uint64_t)(o->lease < 60U ? 60U : o->lease) * 1000U;
        uint64_t t1 = o->has_t1 ? (uint64_t)o->t1 * 1000U : lease / 2U;
        uint64_t t2 = o->has_t2 ? (uint64_t)o->t2 * 1000U : lease * 7U / 8U;
        if (t2 >= lease || t2 <= t1) { t1 = lease / 2U; t2 = lease * 7U / 8U; }
        dhcp.t1 = now + t1; dhcp.t2 = now + t2; dhcp.expiry = now + lease;
    }
    dhcp.state = DHCP_BOUND;
    dhcp.attempts = 0;
}

void net_dhcp_input(const unsigned char* m, unsigned int bytes) {
    if (dhcp.state == DHCP_OFF || dhcp.state == DHCP_BOUND || bytes < 240) return;
    if (m[0] != 2 || m[1] != 1 || m[2] != 6 || net_get32(m + 4) != dhcp.xid ||
        memcmp(m + 28, net_interface.mac, 6) || net_get32(m + 236) != COOKIE) return;
    Options o;
    if (!parse(m + 240, bytes - 240, &o)) return;
    uint32_t yiaddr = net_get32(m + 16);
    if (dhcp.state == DHCP_SELECTING) {
        if (o.type != OFFER || !usable(yiaddr) || !usable(o.server)) return;
        dhcp.offered = yiaddr;
        dhcp.server = o.server;
        dhcp.state = DHCP_REQUESTING;
        dhcp.attempts = 0;
        dhcp.next = net_now();
        net_dhcp_poll();
        return;
    }
    if (dhcp.state == DHCP_REQUESTING && o.server != dhcp.server) return;
    if (o.type == NAK) { lost(); return; }
    if (o.type != ACK || !usable(yiaddr) || !o.has_lease) return;
    if (dhcp.state == DHCP_REQUESTING && yiaddr != dhcp.offered) return;
    if (dhcp.state == DHCP_REBINDING && usable(o.server)) dhcp.server = o.server;
    bind_lease(yiaddr, &o);
}
