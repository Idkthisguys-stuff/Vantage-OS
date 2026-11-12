/* TCP client connections (RFC 9293): active open only, MSS option, RFC 6298
 * retransmission timing, slow start/congestion avoidance with fast
 * retransmit, zero-window probing and RFC 5961 reset/SYN checks. In-order
 * receive only: out-of-order data is discarded and acknowledged. */
#include "internal.h"

#define FIN 0x01U
#define SYN 0x02U
#define RST 0x04U
#define PSH 0x08U
#define ACK 0x10U

#define RTO_MIN 1000U
#define RTO_MAX 60000U
#define SYN_RETRIES 5U
#define DATA_RETRIES 8U
#define TIME_WAIT_MS 30000U
#define FIN_WAIT_2_MS 60000U
#define ORPHAN_MS 120000U
#define DEFAULT_MSS 536U
#define CWND_MAX 131072U

static uint32_t minimum(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t rx_free(const NetTcp* t) { return TCP_RX_BYTES - t->rx_used; }

static void checksum(unsigned char* segment, unsigned int bytes, uint32_t remote) {
    net_put16(segment + 16, 0);
    net_put16(segment + 16, net_fold(net_sum(segment, bytes,
                                             net_pseudo(net_interface.address, remote, IP_TCP, bytes))));
}

/* Payload comes from the transmit ring, `offset` bytes after snd_una. */
static void transmit(NetSocket* s, uint32_t sequence, unsigned int flags, unsigned int offset, unsigned int bytes) {
    NetTcp* t = &s->u.tcp;
    unsigned char segment[TCP_HEADER + 4 + TCP_MSS];
    unsigned int header = (flags & SYN) ? TCP_HEADER + 4 : TCP_HEADER;
    uint32_t window = minimum(rx_free(t), 65535U);
    net_put16(segment, s->local_port); net_put16(segment + 2, s->remote_port);
    net_put32(segment + 4, sequence); net_put32(segment + 8, (flags & ACK) ? t->rcv_nxt : 0);
    segment[12] = (unsigned char)((header / 4U) << 4); segment[13] = (unsigned char)flags;
    net_put16(segment + 14, window); net_put16(segment + 18, 0);
    if (flags & SYN) { segment[20] = 2; segment[21] = 4; net_put16(segment + 22, TCP_MSS); }
    for (unsigned int i = 0; i < bytes; i++) segment[header + i] = t->tx[(t->tx_head + offset + i) % TCP_TX_BYTES];
    checksum(segment, header + bytes, s->remote);
    (void)net_ip_send(net_interface.address, s->remote, IP_TCP, segment, header + bytes);
    if (flags & ACK) t->advertised = window;
}

static void reply_reset(uint32_t remote, uint16_t remote_port, uint16_t port,
                        uint32_t sequence, uint32_t acknowledgement, unsigned int flags) {
    unsigned char segment[TCP_HEADER] = {0};
    net_put16(segment, port); net_put16(segment + 2, remote_port);
    net_put32(segment + 4, sequence); net_put32(segment + 8, acknowledgement);
    segment[12] = 0x50; segment[13] = (unsigned char)flags;
    checksum(segment, sizeof(segment), remote);
    (void)net_ip_send(net_interface.address, remote, IP_TCP, segment, sizeof(segment));
}

static int sending(int state) {
    return state == TCP_ESTABLISHED || state == TCP_CLOSE_WAIT || state == TCP_FIN_WAIT_1 ||
           state == TCP_CLOSING || state == TCP_LAST_ACK;
}

static void closed(NetSocket* s, int error) {
    NetTcp* t = &s->u.tcp;
    t->state = TCP_CLOSED;
    t->error = error;
    t->timer = t->wait = 0;
    if (s->state == SOCKET_ORPHAN) net_socket_free(s);
}

void net_tcp_fail(NetSocket* s, int error) { closed(s, error); }

static void output(NetSocket* s, int acknowledge) {
    NetTcp* t = &s->u.tcp;
    uint64_t now = net_now();
    int sent = 0;
    if (sending(t->state)) {
        while (!t->fin_sent) {
            uint32_t flight = t->snd_nxt - t->snd_una;
            if (flight >= t->tx_used) break;
            uint32_t window = minimum(t->snd_wnd, t->cwnd);
            if (!t->snd_wnd && t->probe) window = 1;
            if (flight >= window) break;
            uint32_t bytes = minimum(minimum(t->tx_used - flight, window - flight), t->mss);
            transmit(s, t->snd_nxt, ACK | PSH, flight, bytes);
            if (!t->rtt_active) { t->rtt_active = 1; t->rtt_seq = t->snd_nxt; t->rtt_start = now; }
            t->snd_nxt += bytes;
            if (net_before(t->snd_max, t->snd_nxt)) t->snd_max = t->snd_nxt;
            t->probe = 0;
            sent = 1;
            if (!t->timer) t->timer = now + t->rto;
        }
        if (t->fin_queued && !t->fin_sent && t->snd_nxt - t->snd_una == t->tx_used) {
            transmit(s, t->snd_nxt, FIN | ACK, 0, 0);
            t->fin_sent = t->fin_out = 1;
            t->fin_seq = t->snd_nxt++;
            if (net_before(t->snd_max, t->snd_nxt)) t->snd_max = t->snd_nxt;
            sent = 1;
            if (!t->timer) t->timer = now + t->rto;
            if (t->state == TCP_ESTABLISHED) t->state = TCP_FIN_WAIT_1;
            else if (t->state == TCP_CLOSE_WAIT) t->state = TCP_LAST_ACK;
        }
        /* Data waits behind a zero window with nothing in flight: probe later. */
        if (!t->snd_wnd && !t->timer && t->snd_max == t->snd_una && t->tx_used) t->timer = now + t->rto;
    }
    if (acknowledge && !sent) transmit(s, t->snd_nxt, ACK, 0, 0);
}

static void sample(NetTcp* t, uint64_t elapsed) {
    uint32_t r = elapsed > RTO_MAX ? RTO_MAX : (uint32_t)elapsed;
    if (!t->rtt_valid) {
        t->srtt = r; t->rttvar = r / 2U; t->rtt_valid = 1;
    } else {
        uint32_t delta = t->srtt > r ? t->srtt - r : r - t->srtt;
        t->rttvar = (3U * t->rttvar + delta) / 4U;
        t->srtt = (7U * t->srtt + r) / 8U;
    }
    uint32_t rto = t->srtt + (4U * t->rttvar > 10U ? 4U * t->rttvar : 10U);
    t->rto = rto < RTO_MIN ? RTO_MIN : rto > RTO_MAX ? RTO_MAX : rto;
}

int net_tcp_connect(NetSocket* s) {
    NetTcp* t = &s->u.tcp;
    uint64_t now = net_now();
    t->iss = net_random() + (uint32_t)(now * 250U);
    t->snd_una = t->iss;
    t->snd_nxt = t->snd_max = t->iss + 1U;
    t->mss = DEFAULT_MSS;
    t->rto = RTO_MIN;
    t->cwnd = 3U * DEFAULT_MSS;
    t->ssthresh = 65535U;
    t->state = TCP_SYN_SENT;
    t->rtt_active = 1; t->rtt_seq = t->iss; t->rtt_start = now;
    transmit(s, t->iss, SYN, 0, 0);
    t->timer = now + t->rto;
    return NET_AGAIN;
}

static uint32_t peer_mss(const unsigned char* g, unsigned int header) {
    for (unsigned int i = TCP_HEADER; i < header;) {
        unsigned int kind = g[i];
        if (kind == 0) break;
        if (kind == 1) { i++; continue; }
        if (i + 1 >= header) break;
        unsigned int length = g[i + 1];
        if (length < 2 || length > header - i) break;
        if (kind == 2 && length == 4) {
            uint32_t mss = net_get16(g + i + 2);
            return mss < 64U ? 64U : mss > TCP_MSS ? TCP_MSS : mss;
        }
        i += length;
    }
    return DEFAULT_MSS;
}

static void syn_sent(NetSocket* s, const unsigned char* g, unsigned int header, uint32_t sequence,
                     uint32_t acknowledgement, unsigned int flags, uint32_t window) {
    NetTcp* t = &s->u.tcp;
    if ((flags & ACK) && (!net_before(t->iss, acknowledgement) || net_before(t->snd_nxt, acknowledgement))) {
        if (!(flags & RST)) reply_reset(s->remote, s->remote_port, s->local_port, acknowledgement, 0, RST);
        return;
    }
    if (flags & RST) {
        if (flags & ACK) closed(s, NET_REFUSED);
        return;
    }
    if (!(flags & SYN) || !(flags & ACK)) return;  /* Simultaneous open is not supported. */
    t->rcv_nxt = sequence + 1U;
    t->snd_una = acknowledgement;
    t->snd_max = t->snd_nxt;
    t->mss = peer_mss(g, header);
    t->snd_wnd = window; t->snd_wl1 = sequence; t->snd_wl2 = acknowledgement;
    if (t->rtt_active && !t->retries) sample(t, net_now() - t->rtt_start);
    t->rtt_active = 0;
    t->retries = 0;
    t->timer = 0;
    t->cwnd = 3U * t->mss;
    t->state = TCP_ESTABLISHED;
    output(s, 1);
}

/* Returns 1 to continue with the segment, 0 to drop it, -1 if closed. */
static int acknowledge(NetSocket* s, uint32_t sequence, uint32_t ack, uint32_t window, unsigned int bytes) {
    NetTcp* t = &s->u.tcp;
    uint64_t now = net_now();
    /* snd_max is the highest sequence ever sent; a retransmission timeout
     * rewinds snd_nxt, and ACKs for data sent before it remain valid. */
    if (net_before(t->snd_max, ack)) { output(s, 1); return 0; }
    if (net_before(t->snd_una, ack)) {
        uint32_t acked = ack - t->snd_una;
        int fin_acked = t->fin_out && !net_before(ack, t->fin_seq + 1U);
        uint32_t data = minimum(acked - (fin_acked ? 1U : 0U), t->tx_used);
        t->tx_head = (t->tx_head + data) % TCP_TX_BYTES;
        t->tx_used -= data;
        t->snd_una = ack;
        if (net_before(t->snd_nxt, ack)) t->snd_nxt = ack;
        if (fin_acked) t->fin_sent = 1;
        if (t->rtt_active && net_before(t->rtt_seq, ack)) { sample(t, now - t->rtt_start); t->rtt_active = 0; }
        t->retries = 0;
        t->dupacks = 0;
        t->cwnd += t->cwnd < t->ssthresh ? t->mss : (t->mss * t->mss / t->cwnd ? t->mss * t->mss / t->cwnd : 1U);
        if (t->cwnd > CWND_MAX) t->cwnd = CWND_MAX;
        t->timer = t->snd_una == t->snd_max ? 0 : now + t->rto;
        if (fin_acked) {
            if (t->state == TCP_FIN_WAIT_1) {
                t->state = TCP_FIN_WAIT_2;
                if (s->state == SOCKET_ORPHAN) t->wait = now + FIN_WAIT_2_MS;
            } else if (t->state == TCP_CLOSING) {
                t->state = TCP_TIME_WAIT; t->wait = now + TIME_WAIT_MS;
            } else if (t->state == TCP_LAST_ACK) {
                closed(s, 0);
                return -1;
            }
        }
    } else if (ack == t->snd_una && !bytes && window == t->snd_wnd && t->snd_max != t->snd_una) {
        if (++t->dupacks == 3) {
            uint32_t flight = t->snd_max - t->snd_una;
            t->ssthresh = flight / 2U > 2U * t->mss ? flight / 2U : 2U * t->mss;
            t->cwnd = t->ssthresh;
            t->rtt_active = 0;
            if (t->tx_used) transmit(s, t->snd_una, ACK | PSH, 0, minimum(t->tx_used, t->mss));
            else if (t->fin_out) transmit(s, t->fin_seq, FIN | ACK, 0, 0);
        }
    }
    if (net_before(t->snd_wl1, sequence) || (t->snd_wl1 == sequence && !net_before(ack, t->snd_wl2))) {
        uint32_t previous = t->snd_wnd;
        t->snd_wnd = window; t->snd_wl1 = sequence; t->snd_wl2 = ack;
        if (!previous && window) {
            t->probe = 0;
            if (t->snd_una == t->snd_max) t->timer = 0;
        }
    }
    return 1;
}

static NetSocket* find(uint32_t remote, uint16_t remote_port, uint16_t port) {
    for (unsigned int i = 0; i < NET_SOCKET_LIMIT; i++) {
        NetSocket* s = &net_sockets[i];
        if (s->state != SOCKET_FREE && s->type == NET_TCP && s->u.tcp.state != TCP_CLOSED &&
            s->remote == remote && s->remote_port == remote_port && s->local_port == port) return s;
    }
    return NULL;
}

void net_tcp_input(uint32_t source, uint32_t destination, const unsigned char* g, unsigned int bytes) {
    if (bytes < TCP_HEADER) return;
    unsigned int header = (g[12] >> 4) * 4U;
    if (header < TCP_HEADER || header > bytes ||
        net_fold(net_sum(g, bytes, net_pseudo(source, destination, IP_TCP, bytes)))) return;
    uint16_t remote_port = net_get16(g), port = net_get16(g + 2);
    uint32_t sequence = net_get32(g + 4), ack = net_get32(g + 8), window = net_get16(g + 14);
    unsigned int flags = g[13] & 0x3fU;
    const unsigned char* data = g + header;
    unsigned int length = bytes - header;
    NetSocket* s = find(source, remote_port, port);
    if (!s) {
        if (flags & RST) return;
        if (flags & ACK) reply_reset(source, remote_port, port, ack, 0, RST);
        else reply_reset(source, remote_port, port, 0,
                         sequence + length + ((flags & SYN) ? 1U : 0U) + ((flags & FIN) ? 1U : 0U), RST | ACK);
        return;
    }
    NetTcp* t = &s->u.tcp;
    if (t->state == TCP_SYN_SENT) { syn_sent(s, g, header, sequence, ack, flags, window); return; }

    uint32_t space = rx_free(t), span = length + ((flags & SYN) ? 1U : 0U) + ((flags & FIN) ? 1U : 0U);
    int acceptable;
    if (!span) acceptable = space ? !net_before(sequence, t->rcv_nxt) && net_before(sequence, t->rcv_nxt + space)
                                  : sequence == t->rcv_nxt;
    else if (!space) acceptable = 0;
    else {
        uint32_t last = sequence + span - 1U;
        acceptable = (!net_before(sequence, t->rcv_nxt) && net_before(sequence, t->rcv_nxt + space)) ||
                     (!net_before(last, t->rcv_nxt) && net_before(last, t->rcv_nxt + space));
    }
    if (!acceptable) {
        if (flags & RST) return;
        if (t->state == TCP_TIME_WAIT && (flags & FIN)) t->wait = net_now() + TIME_WAIT_MS;
        /* A closed receive window still accepts acknowledgements. */
        if (sequence == t->rcv_nxt && (flags & ACK) && !(flags & SYN) &&
            acknowledge(s, sequence, ack, window, length) < 0) return;
        output(s, 1);
        return;
    }
    if (flags & RST) {
        if (sequence == t->rcv_nxt) closed(s, NET_RESET);
        else output(s, 1);  /* Challenge ACK. */
        return;
    }
    if (flags & SYN) { output(s, 1); return; }
    if (!(flags & ACK) || acknowledge(s, sequence, ack, window, length) <= 0) return;

    int reply = 0;
    uint32_t end = sequence + length;
    if (length) {
        reply = 1;
        if (t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 || t->state == TCP_FIN_WAIT_2) {
            if (s->state == SOCKET_ORPHAN) {
                /* RFC 1122 4.2.2.13: nobody will read this data. */
                transmit(s, t->snd_nxt, RST | ACK, 0, 0);
                closed(s, NET_RESET);
                return;
            }
            uint32_t skip = net_before(sequence, t->rcv_nxt) ? t->rcv_nxt - sequence : 0;
            if (skip < length && sequence + skip == t->rcv_nxt) {
                uint32_t count = minimum(length - skip, rx_free(t));
                for (uint32_t i = 0; i < count; i++)
                    t->rx[(t->rx_head + t->rx_used + i) % TCP_RX_BYTES] = data[skip + i];
                t->rx_used += count;
                t->rcv_nxt += count;
            }
        }
    }
    if (flags & FIN) {
        reply = 1;
        if (!t->fin_received && t->rcv_nxt == end) {
            t->rcv_nxt++;
            t->fin_received = 1;
            if (t->state == TCP_ESTABLISHED) t->state = TCP_CLOSE_WAIT;
            else if (t->state == TCP_FIN_WAIT_1) t->state = TCP_CLOSING;
            else if (t->state == TCP_FIN_WAIT_2) { t->state = TCP_TIME_WAIT; t->wait = net_now() + TIME_WAIT_MS; }
        }
    }
    output(s, reply);
}

void net_tcp_timer(NetSocket* s) {
    NetTcp* t = &s->u.tcp;
    uint64_t now = net_now();
    if (s->state == SOCKET_ORPHAN && t->linger && now >= t->linger) { closed(s, NET_TIMEOUT); return; }
    if (t->wait && now >= t->wait) {
        t->wait = 0;
        if (t->state == TCP_TIME_WAIT || t->state == TCP_FIN_WAIT_2) { closed(s, 0); return; }
    }
    if (!t->timer || now < t->timer) return;
    t->timer = 0;
    if (t->state == TCP_SYN_SENT) {
        if (t->retries >= SYN_RETRIES) { closed(s, NET_TIMEOUT); return; }
        t->retries++;
        t->rto = minimum(t->rto * 2U, RTO_MAX);
        t->rtt_active = 0;
        transmit(s, t->iss, SYN, 0, 0);
        t->timer = now + t->rto;
        return;
    }
    if (!sending(t->state)) return;
    uint32_t flight = t->snd_max - t->snd_una;
    int persist = !t->snd_wnd && t->tx_used;
    if (!flight && !persist) return;
    /* Probes of a closed window are answered: they never abort the peer. */
    if (!persist && ++t->retries > DATA_RETRIES) {
        transmit(s, t->snd_nxt, RST | ACK, 0, 0);
        closed(s, NET_TIMEOUT);
        return;
    }
    if (flight) {
        t->ssthresh = flight / 2U > 2U * t->mss ? flight / 2U : 2U * t->mss;
        t->cwnd = t->mss;
    }
    t->snd_nxt = t->snd_una;
    t->fin_sent = 0;
    t->rtt_active = 0;
    t->dupacks = 0;
    t->rto = minimum(t->rto * 2U, RTO_MAX);
    if (persist) t->probe = 1;
    output(s, 0);
    if (!t->timer) t->timer = now + t->rto;
}

int net_tcp_write(NetSocket* s, const unsigned char* data, unsigned int bytes) {
    NetTcp* t = &s->u.tcp;
    if (t->state == TCP_SYN_SENT) return NET_AGAIN;
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT)
        return t->error ? t->error : s->remote ? NET_RESET : NET_BAD;
    if (!bytes) return 0;
    uint32_t space = TCP_TX_BYTES - t->tx_used;
    if (!space) return NET_AGAIN;
    bytes = minimum(bytes, space);
    for (unsigned int i = 0; i < bytes; i++) t->tx[(t->tx_head + t->tx_used + i) % TCP_TX_BYTES] = data[i];
    t->tx_used += bytes;
    output(s, 0);
    return (int)bytes;
}

int net_tcp_read(NetSocket* s, unsigned char* data, unsigned int capacity) {
    NetTcp* t = &s->u.tcp;
    if (t->rx_used) {
        uint32_t count = minimum(capacity, t->rx_used);
        for (uint32_t i = 0; i < count; i++) data[i] = t->rx[(t->rx_head + i) % TCP_RX_BYTES];
        t->rx_head = (t->rx_head + count) % TCP_RX_BYTES;
        t->rx_used -= count;
        /* Announce reopened space once at least a full segment is available. */
        if ((t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 || t->state == TCP_FIN_WAIT_2) &&
            rx_free(t) >= t->advertised + t->mss) output(s, 1);
        return (int)count;
    }
    if (t->fin_received) return 0;
    if (t->error) return t->error;
    if (t->state == TCP_SYN_SENT || t->state == TCP_ESTABLISHED ||
        t->state == TCP_FIN_WAIT_1 || t->state == TCP_FIN_WAIT_2) return NET_AGAIN;
    return s->remote ? 0 : NET_BAD;
}

void net_tcp_close(NetSocket* s, int abort) {
    NetTcp* t = &s->u.tcp;
    if (t->state == TCP_CLOSED || t->state == TCP_SYN_SENT) { net_socket_free(s); return; }
    if (abort || t->rx_used) {
        if (t->state != TCP_TIME_WAIT) transmit(s, t->snd_nxt, RST | ACK, 0, 0);
        net_socket_free(s);
        return;
    }
    uint64_t now = net_now();
    s->state = SOCKET_ORPHAN;
    t->linger = now + ORPHAN_MS;
    if (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) { t->fin_queued = 1; output(s, 0); }
    else if (t->state == TCP_FIN_WAIT_2) t->wait = now + FIN_WAIT_2_MS;
}

int net_tcp_state(const NetSocket* s) {
    switch (s->u.tcp.state) {
    case TCP_CLOSED: return NET_TCP_CLOSED;
    case TCP_SYN_SENT: return NET_TCP_CONNECTING;
    case TCP_ESTABLISHED: return NET_TCP_ESTABLISHED;
    case TCP_CLOSE_WAIT: return NET_TCP_PEER_CLOSED;
    default: return NET_TCP_CLOSING;
    }
}
