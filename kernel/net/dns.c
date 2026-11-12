/* Stub resolver for IPv4 A records over UDP (RFC 1035), with a small cache. */
#include "internal.h"

#define DNS_ENTRIES 12U
#define DNS_TRIES 4U
#define DNS_FAILED_MS 5000U
#define DNS_TTL_MIN 5U
#define DNS_TTL_MAX 3600U
#define NAME_TEXT 254U

enum { DNS_FREE, DNS_PENDING, DNS_DONE, DNS_FAILED };

typedef struct {
    int state, error;
    char name[NAME_TEXT];
    uint32_t address;
    uint16_t id, port;
    unsigned int attempt;
    uint64_t next, expires, used;
} DnsEntry;

static DnsEntry entries[DNS_ENTRIES];

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }
static int differ(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a != *b;
}

/* Canonical lowercase name without a trailing dot; LDH and '_' labels. */
static int normalize(const char* name, char out[NAME_TEXT]) {
    unsigned int length = 0, label = 0;
    for (; name[length] && length < NET_NAME_MAX; length++) {
        char c = name[length];
        if (c == '.') {
            if (!label) return 0;
            label = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_') || ++label > 63) return 0;
        }
        if (length >= NAME_TEXT - 1) return 0;
        out[length] = lower(c);
    }
    if (length == NET_NAME_MAX) return 0;
    if (length && out[length - 1] == '.') length--;
    if (!length || length > 253) return 0;
    out[length] = 0;
    return 1;
}

int net_dns_port_in_use(uint16_t port) {
    for (unsigned int i = 0; i < DNS_ENTRIES; i++)
        if (entries[i].state == DNS_PENDING && entries[i].port == port) return 1;
    return 0;
}

static unsigned int servers(uint32_t list[2]) {
    unsigned int count = 0;
    for (unsigned int i = 0; i < 2; i++) if (net_interface.dns[i]) list[count++] = net_interface.dns[i];
    return count;
}

static void finish(DnsEntry* e, int error, uint32_t address, uint32_t ttl) {
    uint64_t now = net_now();
    e->error = error;
    e->address = address;
    if (error) { e->state = DNS_FAILED; e->expires = now + DNS_FAILED_MS; return; }
    if (ttl < DNS_TTL_MIN) ttl = DNS_TTL_MIN;
    if (ttl > DNS_TTL_MAX) ttl = DNS_TTL_MAX;
    e->state = DNS_DONE;
    e->expires = now + ttl * 1000ULL;
}

static void query(DnsEntry* e) {
    uint32_t list[2];
    unsigned int count = servers(list);
    if (!count) { finish(e, NET_UNREACHABLE, 0, 0); return; }
    unsigned char m[12 + NAME_TEXT + 2 + 4] = {0};
    net_put16(m, e->id); net_put16(m + 2, 0x0100); net_put16(m + 4, 1);
    unsigned int at = 12;
    for (const char* label = e->name; *label;) {
        unsigned int length = 0;
        while (label[length] && label[length] != '.') length++;
        m[at++] = (unsigned char)length;
        memcpy(m + at, label, length);
        at += length;
        label += length;
        if (*label == '.') label++;
    }
    m[at++] = 0;
    net_put16(m + at, 1); net_put16(m + at + 2, 1);
    at += 4;
    (void)net_udp_send(net_interface.address, list[e->attempt % count], e->port, 53, m, at);
    e->attempt++;
    e->next = net_now() + 1000ULL * e->attempt;
}

int net_dns_resolve(const char* name, uint32_t* address) {
    char canonical[NAME_TEXT];
    if (!normalize(name, canonical)) return NET_BAD;
    uint64_t now = net_now();
    DnsEntry* slot = NULL;
    for (unsigned int i = 0; i < DNS_ENTRIES; i++) {
        DnsEntry* e = &entries[i];
        if (e->state != DNS_FREE && e->state != DNS_PENDING && now >= e->expires) e->state = DNS_FREE;
        if (e->state == DNS_FREE) continue;
        if (differ(e->name, canonical)) continue;
        e->used = now;
        if (e->state == DNS_DONE) { *address = e->address; return 1; }
        return e->state == DNS_PENDING ? NET_AGAIN : e->error;
    }
    for (unsigned int i = 0; i < DNS_ENTRIES; i++) {
        DnsEntry* e = &entries[i];
        if (e->state == DNS_FREE) { slot = e; break; }
        if (e->state == DNS_DONE && (!slot || e->used < slot->used)) slot = e;
    }
    if (!slot) return NET_BUSY;
    uint16_t port = net_port_ephemeral();
    if (!port) return NET_BUSY;
    memset(slot, 0, sizeof(*slot));
    memcpy(slot->name, canonical, sizeof(canonical));
    slot->state = DNS_PENDING;
    slot->id = (uint16_t)net_random();
    slot->port = port;
    slot->used = now;
    query(slot);
    return slot->state == DNS_FAILED ? slot->error : NET_AGAIN;
}

void net_dns_reset(void) {
    for (unsigned int i = 0; i < DNS_ENTRIES; i++) {
        if (entries[i].state == DNS_PENDING) finish(&entries[i], NET_OFFLINE, 0, 0);
        else if (entries[i].state == DNS_DONE) entries[i].state = DNS_FREE;
    }
}

void net_dns_poll(void) {
    uint64_t now = net_now();
    for (unsigned int i = 0; i < DNS_ENTRIES; i++) {
        DnsEntry* e = &entries[i];
        if (e->state == DNS_PENDING && now >= e->next) {
            if (!net_link_ready() || !net_interface.bound) finish(e, NET_OFFLINE, 0, 0);
            else if (e->attempt >= DNS_TRIES) finish(e, NET_TIMEOUT, 0, 0);
            else query(e);
        } else if ((e->state == DNS_DONE || e->state == DNS_FAILED) && now >= e->expires) {
            e->state = DNS_FREE;
        }
    }
}

/* Decodes a possibly compressed name into canonical lowercase text. *end is
 * the offset after the name at its original position. */
static int name_at(const unsigned char* m, unsigned int bytes, unsigned int offset,
                   char out[NAME_TEXT], unsigned int* end) {
    unsigned int length = 0, jumps = 0;
    int jumped = 0;
    for (;;) {
        if (offset >= bytes) return 0;
        unsigned int label = m[offset];
        if ((label & 0xc0U) == 0xc0U) {
            if (offset + 1 >= bytes || ++jumps > 16) return 0;
            if (!jumped) *end = offset + 2;
            jumped = 1;
            offset = ((label & 0x3fU) << 8) | m[offset + 1];
            continue;
        }
        if (label & 0xc0U) return 0;
        offset++;
        if (!label) break;
        if (label > bytes - offset || length + label + 1 > 253) return 0;
        if (length) out[length++] = '.';
        for (unsigned int i = 0; i < label; i++) out[length++] = lower((char)m[offset + i]);
        offset += label;
    }
    if (!jumped) *end = offset;
    out[length] = 0;
    return 1;
}

int net_dns_input(uint32_t source, uint16_t source_port, uint16_t port,
                  const unsigned char* m, unsigned int bytes) {
    DnsEntry* e = NULL;
    for (unsigned int i = 0; i < DNS_ENTRIES && !e; i++)
        if (entries[i].state == DNS_PENDING && entries[i].port == port) e = &entries[i];
    if (!e) return 0;
    uint32_t list[2];
    unsigned int count = servers(list), known = 0;
    for (unsigned int i = 0; i < count; i++) known |= list[i] == source;
    if (source_port != 53 || !known || bytes < 12 || net_get16(m) != e->id) return 1;
    unsigned int flags = net_get16(m + 2);
    if (!(flags & 0x8000U) || (flags & 0x7800U) || net_get16(m + 4) != 1) return 1;
    char name[NAME_TEXT], target[NAME_TEXT];
    unsigned int at = 0;
    if (!name_at(m, bytes, 12, name, &at) || at + 4 > bytes || differ(name, e->name) ||
        net_get16(m + at) != 1 || net_get16(m + at + 2) != 1) return 1;
    at += 4;
    unsigned int rcode = flags & 15U;
    if (rcode == 3) { finish(e, NET_NOT_FOUND, 0, 0); return 1; }
    if (rcode == 2 || rcode == 5) {
        /* Try the next server, if any attempts remain. */
        e->next = net_now();
        return 1;
    }
    if (rcode) { finish(e, NET_IO, 0, 0); return 1; }
    memcpy(target, e->name, sizeof(target));
    unsigned int answers = net_get16(m + 6), aliases = 0;
    for (unsigned int i = 0; i < answers; i++) {
        unsigned int next = 0;
        if (!name_at(m, bytes, at, name, &next) || next + 10 > bytes) break;
        unsigned int type = net_get16(m + next), class_ = net_get16(m + next + 2);
        uint32_t ttl = net_get32(m + next + 4);
        unsigned int length = net_get16(m + next + 8);
        unsigned int data = next + 10;
        if (length > bytes - data) break;
        if (class_ == 1 && !differ(name, target)) {
            if (type == 1 && length == 4) {
                uint32_t address = net_get32(m + data);
                finish(e, 0, address, ttl > 0x7fffffffU ? 0 : ttl);
                return 1;
            }
            if (type == 5 && ++aliases <= 8) {
                unsigned int unused;
                if (!name_at(m, bytes, data, target, &unused)) break;
            }
        }
        at = data + length;
    }
    finish(e, NET_NOT_FOUND, 0, 0);
    return 1;
}
