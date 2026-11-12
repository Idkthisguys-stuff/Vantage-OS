/* Process-owned UDP, ICMP echo and TCP client sockets. Handles carry a slot
 * generation, so a stale handle never reaches a reused slot. */
#include "internal.h"

NetSocket net_sockets[NET_SOCKET_LIMIT];

static uint64_t handle_of(const NetSocket* s) {
    return ((uint64_t)s->generation << 8) | (uint64_t)(s - net_sockets + 1);
}

static NetSocket* lookup(uint64_t owner, uint64_t handle) {
    unsigned int slot = (unsigned int)(handle & 255U);
    if (!slot || slot > NET_SOCKET_LIMIT || (handle >> 40)) return NULL;
    NetSocket* s = &net_sockets[slot - 1];
    return s->state == SOCKET_OPEN && s->owner == owner && handle_of(s) == handle ? s : NULL;
}

void net_socket_free(NetSocket* s) {
    uint32_t generation = s->generation;
    memset(s, 0, sizeof(*s));
    s->generation = generation;
}

int net_socket_port_in_use(uint16_t port) {
    for (unsigned int i = 0; i < NET_SOCKET_LIMIT; i++)
        if (net_sockets[i].state != SOCKET_FREE && net_sockets[i].local_port == port) return 1;
    return 0;
}

static NetSocket* allocate(void) {
    NetSocket* reclaim = NULL;
    for (unsigned int i = 0; i < NET_SOCKET_LIMIT; i++) {
        NetSocket* s = &net_sockets[i];
        if (s->state == SOCKET_FREE) return s;
        /* RFC 6191-style reuse: the oldest TIME-WAIT orphan yields its slot. */
        if (s->state == SOCKET_ORPHAN && s->u.tcp.state == TCP_TIME_WAIT &&
            (!reclaim || s->u.tcp.wait < reclaim->u.tcp.wait)) reclaim = s;
    }
    if (reclaim) net_socket_free(reclaim);
    return reclaim;
}

static int identifier_used(uint16_t identifier) {
    for (unsigned int i = 0; i < NET_SOCKET_LIMIT; i++)
        if (net_sockets[i].state == SOCKET_OPEN && net_sockets[i].type == NET_ICMP &&
            net_sockets[i].identifier == identifier) return 1;
    return 0;
}

static int64_t open_socket(uint64_t owner, uint32_t type) {
    if (type != NET_UDP && type != NET_TCP && type != NET_ICMP) return NET_BAD;
    NetSocket* s = allocate();
    if (!s) return NET_NO_SPACE;
    uint16_t port = 0, identifier = 0;
    if (type == NET_UDP && !(port = net_port_ephemeral())) return NET_NO_SPACE;
    if (type == NET_ICMP) {
        unsigned int tries = 0;
        do identifier = (uint16_t)net_random(); while ((!identifier || identifier_used(identifier)) && ++tries < 64);
        if (tries == 64) return NET_NO_SPACE;
    }
    uint32_t generation = s->generation + 1U;
    if (generation >= (1U << 31)) generation = 1;
    memset(s, 0, sizeof(*s));
    s->generation = generation;
    s->state = SOCKET_OPEN;
    s->type = type;
    s->owner = owner;
    s->local_port = port;
    s->identifier = identifier;
    return (int64_t)handle_of(s);
}

static void enqueue(NetSocket* s, uint32_t address, uint16_t port, const unsigned char* data, unsigned int bytes) {
    if (bytes > DGRAM_MAX) return;
    if (s->u.dgram.count == DGRAM_SLOTS) { s->u.dgram.dropped++; return; }
    NetDatagram* d = &s->u.dgram.queue[(s->u.dgram.head + s->u.dgram.count) % DGRAM_SLOTS];
    d->address = address; d->port = port; d->bytes = (uint16_t)bytes;
    if (bytes) memcpy(d->data, data, bytes);
    s->u.dgram.count++;
}

int net_udp_deliver(uint32_t source, uint16_t source_port, uint16_t port,
                    const unsigned char* data, unsigned int bytes) {
    for (unsigned int i = 0; i < NET_SOCKET_LIMIT; i++) {
        NetSocket* s = &net_sockets[i];
        if (s->state != SOCKET_OPEN || s->type != NET_UDP || s->local_port != port) continue;
        if (s->remote && (s->remote != source || s->remote_port != source_port)) return 0;
        enqueue(s, source, source_port, data, bytes);
        return 1;
    }
    return 0;
}

void net_icmp_deliver(uint32_t source, uint16_t identifier, uint16_t sequence,
                      const unsigned char* data, unsigned int bytes) {
    for (unsigned int i = 0; i < NET_SOCKET_LIMIT; i++) {
        NetSocket* s = &net_sockets[i];
        if (s->state == SOCKET_OPEN && s->type == NET_ICMP && s->identifier == identifier &&
            (!s->remote || s->remote == source)) {
            enqueue(s, source, sequence, data, bytes > NET_IO_MAX ? NET_IO_MAX : bytes);
            return;
        }
    }
}

void net_icmp_unreachable(unsigned int code, const unsigned char* ip, unsigned int bytes) {
    if (bytes < IP_HEADER + 8 || (ip[0] >> 4) != 4) return;
    unsigned int header = (ip[0] & 15U) * 4U;
    if (header < IP_HEADER || bytes < header + 8 || net_get32(ip + 12) != net_interface.address) return;
    uint32_t remote = net_get32(ip + 16);
    const unsigned char* t = ip + header;
    uint16_t local = net_get16(t), port = net_get16(t + 2);
    int error = code == 3 ? NET_REFUSED : NET_UNREACHABLE;
    for (unsigned int i = 0; i < NET_SOCKET_LIMIT; i++) {
        NetSocket* s = &net_sockets[i];
        if (s->state == SOCKET_FREE || s->local_port != local || s->remote != remote || s->remote_port != port) continue;
        if (ip[9] == IP_TCP && s->type == NET_TCP && s->u.tcp.state == TCP_SYN_SENT &&
            net_get32(t + 4) == s->u.tcp.iss) net_tcp_fail(s, error);
        else if (ip[9] == IP_UDP && s->type == NET_UDP && s->state == SOCKET_OPEN) s->error = error;
    }
}

void net_socket_poll(void) {
    for (unsigned int i = 0; i < NET_SOCKET_LIMIT; i++)
        if (net_sockets[i].state != SOCKET_FREE && net_sockets[i].type == NET_TCP) net_tcp_timer(&net_sockets[i]);
}

void net_socket_reset(void) {
    for (unsigned int i = 0; i < NET_SOCKET_LIMIT; i++) {
        NetSocket* s = &net_sockets[i];
        if (s->state == SOCKET_FREE || s->type != NET_TCP) continue;
        if (s->state == SOCKET_ORPHAN) net_socket_free(s);
        else if (s->u.tcp.state != TCP_CLOSED) net_tcp_fail(s, NET_RESET);
    }
}

void net_release(uint64_t owner) {
    if (!owner) return;
    for (unsigned int i = 0; i < NET_SOCKET_LIMIT; i++) {
        NetSocket* s = &net_sockets[i];
        if (s->state != SOCKET_OPEN || s->owner != owner) continue;
        if (s->type == NET_TCP) net_tcp_close(s, 0);
        else net_socket_free(s);
    }
}

static int pop(NetSocket* s, NetSocketCall* call) {
    if (!s->u.dgram.count) {
        if (s->error) { int error = s->error; s->error = 0; return error; }
        return NET_AGAIN;
    }
    NetDatagram* d = &s->u.dgram.queue[s->u.dgram.head];
    unsigned int bytes = d->bytes < call->length ? d->bytes : call->length;
    memcpy(call->data, d->data, bytes);
    call->address = d->address;
    call->port = d->port;
    call->length = bytes;
    s->u.dgram.head = (s->u.dgram.head + 1) % DGRAM_SLOTS;
    s->u.dgram.count--;
    return (int)bytes;
}

static int send_datagram(NetSocket* s, NetSocketCall* call) {
    if (!net_link_ready() || !net_interface.bound) return NET_OFFLINE;
    if (s->type == NET_ICMP) {
        if (!s->remote) return NET_BAD;
        int result = net_icmp_echo(s->remote, s->identifier, call->port, call->data, call->length);
        return result < 0 ? result : (int)call->length;
    }
    uint32_t address = call->address ? call->address : s->remote;
    uint16_t port = call->address ? call->port : s->remote_port;
    if (!address || !port || call->length > UDP_PAYLOAD_MAX) return NET_BAD;
    int result = net_udp_send(net_interface.address, address, s->local_port, port, call->data, call->length);
    return result < 0 ? result : (int)call->length;
}

int64_t net_socket(uint64_t owner, NetSocketCall* call) {
    if (!owner) return NET_UNAVAILABLE;
    if (call->length > NET_IO_MAX) return NET_BAD;
    if (call->operation == NET_SOCKET_OPEN) return open_socket(owner, call->type);
    NetSocket* s = lookup(owner, call->handle);
    if (!s) return NET_BAD_HANDLE;
    switch (call->operation) {
    case NET_SOCKET_CONNECT:
        if (!call->address || (s->type != NET_ICMP && !call->port)) return NET_BAD;
        if (s->type != NET_TCP) {
            s->remote = call->address;
            s->remote_port = s->type == NET_UDP ? call->port : 0;
            return 0;
        }
        if (s->u.tcp.state == TCP_CLOSED && !s->remote) {
            if (!net_link_ready() || !net_interface.bound) return NET_OFFLINE;
            uint16_t port = net_port_ephemeral();
            if (!port) return NET_NO_SPACE;
            s->remote = call->address;
            s->remote_port = call->port;
            s->local_port = port;
            return net_tcp_connect(s);
        }
        if (s->remote != call->address || s->remote_port != call->port) return NET_BUSY;
        if (s->u.tcp.state == TCP_SYN_SENT) return NET_AGAIN;
        if (s->u.tcp.state == TCP_CLOSED) return s->u.tcp.error ? s->u.tcp.error : NET_RESET;
        return 0;
    case NET_SOCKET_SEND:
        if (s->type == NET_TCP) return net_tcp_write(s, call->data, call->length);
        return send_datagram(s, call);
    case NET_SOCKET_RECEIVE:
        if (s->type == NET_TCP) {
            int result = net_tcp_read(s, call->data, call->length);
            call->length = result > 0 ? (uint32_t)result : 0;
            call->address = s->remote;
            call->port = s->remote_port;
            return result;
        }
        return pop(s, call);
    case NET_SOCKET_CLOSE:
        if (call->flags & ~NET_CLOSE_ABORT) return NET_BAD;
        if (s->type == NET_TCP) net_tcp_close(s, (call->flags & NET_CLOSE_ABORT) != 0);
        else net_socket_free(s);
        return 0;
    case NET_SOCKET_STATUS:
        call->address = s->remote;
        call->port = s->remote_port;
        call->local_port = s->local_port;
        if (s->type == NET_TCP) {
            call->state = net_tcp_state(s);
            call->pending = s->u.tcp.rx_used;
        } else {
            call->state = s->error;
            call->pending = s->u.dgram.count ? s->u.dgram.queue[s->u.dgram.head].bytes : 0;
        }
        return s->type == NET_TCP ? call->state : 0;
    default:
        return NET_BAD;
    }
}
