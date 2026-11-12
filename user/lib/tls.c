/* TLS 1.2 client (vantage/tls.h) on BearSSL. The engine's record buffers are
 * moved to and from the TCP socket without blocking; all state is static. */
#include <vantage/tls.h>
#include <vantage/user.h>
#include "tls_trust.h"

static struct {
    int open, ready, error;
    uint64_t socket;
    br_ssl_client_context client;
    br_x509_minimal_context x509;
    unsigned char buffer[BR_SSL_BUFSIZE_BIDI];
} session;

#ifdef VANTAGE_SIM
static const br_x509_trust_anchor* installed;
static size_t installed_count;
static int installed_set;

void tls_set_trust_anchors(const br_x509_trust_anchor* anchors, size_t count) {
    installed = anchors;
    installed_count = anchors ? count : 0;
    installed_set = anchors != NULL;
}

static const br_x509_trust_anchor* trust(size_t* count) {
    if (!installed_set) return tls_sim_trust_anchors(count);
    *count = installed_count;
    return installed;
}
#else
static const br_x509_trust_anchor* trust(size_t* count) {
    *count = tls_trust_anchor_count;
    return tls_trust_anchors;
}
#endif

static void erase(void* memory, size_t bytes) {
    volatile unsigned char* p = memory;
    while (bytes--) *p++ = 0;
}

/* Validity dates are not checked yet: every certificate counts as current. */
static int any_time(void* context, uint32_t not_before_days, uint32_t not_before_seconds,
                    uint32_t not_after_days, uint32_t not_after_seconds) {
    (void)context; (void)not_before_days; (void)not_before_seconds;
    (void)not_after_days; (void)not_after_seconds;
    return 0;
}

static int fail(int error) {
    if (!session.error) session.error = error;
    return session.error;
}

static int engine_error(void) {
    int error = br_ssl_engine_last_error(&session.client.eng);
    if ((error > BR_ERR_X509_OK && error <= BR_ERR_X509_NOT_TRUSTED) || error == BR_ERR_WRONG_KEY_USAGE)
        return TLS_CERTIFICATE;
    return TLS_PROTOCOL;
}

static uint32_t chunk(size_t length) {
    return length < NET_IO_MAX ? (uint32_t)length : NET_IO_MAX;
}

/* Moves records between the engine and the socket until neither side can
 * make progress: 0 (also after a clean close), or the session's error. */
static int pump(void) {
    br_ssl_engine_context* engine = &session.client.eng;
    if (session.error) return session.error;
    for (;;) {
        unsigned int state = br_ssl_engine_current_state(engine);
        int progress = 0;
        size_t length;
        if (state & BR_SSL_CLOSED)
            return br_ssl_engine_last_error(engine) == BR_ERR_OK ? 0 : fail(engine_error());
        if (state & BR_SSL_SENDREC) {
            unsigned char* data = br_ssl_engine_sendrec_buf(engine, &length);
            int sent = sys_net_send(session.socket, data, chunk(length));
            if (sent > 0) {
                br_ssl_engine_sendrec_ack(engine, (size_t)sent);
                progress = 1;
            } else if (sent < 0 && sent != NET_AGAIN) return fail(sent);
        }
        if (state & BR_SSL_RECVREC) {
            unsigned char* data = br_ssl_engine_recvrec_buf(engine, &length);
            int got = sys_net_receive(session.socket, data, chunk(length));
            if (got > 0) {
                br_ssl_engine_recvrec_ack(engine, (size_t)got);
                progress = 1;
            } else if (got == 0) return fail(TLS_PROTOCOL);  /* End of stream without close_notify. */
            else if (got != NET_AGAIN) return fail(got);
        }
        if (!progress) return 0;
    }
}

int tls_open(uint64_t socket, const char* host) {
    static const uint16_t suites[] = {
        BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
        BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
        BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    };
    static const struct { int id; const br_hash_class* hash; } hashes[] = {
        {br_sha256_ID, &br_sha256_vtable}, {br_sha384_ID, &br_sha384_vtable}, {br_sha512_ID, &br_sha512_vtable},
    };
    br_ssl_client_context* client = &session.client;
    br_ssl_engine_context* engine = &client->eng;
    unsigned char seed[32];
    size_t anchors_count, length = 0;
    const br_x509_trust_anchor* anchors = trust(&anchors_count);
    if (session.open) return TLS_BUSY;
    if (!host) return NET_BAD;
    while (length < NET_NAME_MAX && host[length]) length++;
    if (!length || length >= NET_NAME_MAX) return NET_BAD;
    if (!anchors_count) return TLS_NO_TRUST;
    if (sys_net_random(seed, sizeof(seed)) != (int)sizeof(seed)) {
        erase(seed, sizeof(seed));
        return TLS_NO_RANDOM;
    }
    erase(&session, sizeof(session));
    br_ssl_client_zero(client);
    br_ssl_engine_set_versions(engine, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(engine, suites, sizeof(suites) / sizeof(suites[0]));
    br_x509_minimal_init(&session.x509, &br_sha256_vtable, anchors, anchors_count);
    for (unsigned int i = 0; i < sizeof(hashes) / sizeof(hashes[0]); i++) {
        br_ssl_engine_set_hash(engine, hashes[i].id, hashes[i].hash);
        br_x509_minimal_set_hash(&session.x509, hashes[i].id, hashes[i].hash);
    }
    br_ssl_engine_set_default_rsavrfy(engine);
    br_ssl_engine_set_default_ecdsa(engine);
    br_x509_minimal_set_rsa(&session.x509, br_ssl_engine_get_rsavrfy(engine));
    br_x509_minimal_set_ecdsa(&session.x509, br_ssl_engine_get_ec(engine), br_ssl_engine_get_ecdsa(engine));
    br_x509_minimal_set_minrsa(&session.x509, 256);
    br_x509_minimal_set_time_callback(&session.x509, NULL, any_time);
    br_ssl_engine_set_x509(engine, &session.x509.vtable);
    br_ssl_engine_set_prf_sha256(engine, &br_tls12_sha256_prf);
    br_ssl_engine_set_prf_sha384(engine, &br_tls12_sha384_prf);
    br_ssl_engine_set_default_aes_gcm(engine);
    br_ssl_engine_set_default_chapol(engine);
    br_ssl_engine_add_flags(engine, BR_OPT_NO_RENEGOTIATION);
    br_ssl_engine_set_buffer(engine, session.buffer, sizeof(session.buffer), 1);
    br_ssl_engine_inject_entropy(engine, seed, sizeof(seed));
    erase(seed, sizeof(seed));
    if (!br_ssl_client_reset(client, host, 0)) {
        erase(&session, sizeof(session));
        return TLS_PROTOCOL;
    }
    session.socket = socket;
    session.open = 1;
    return 0;
}

int tls_handshake(void) {
    if (!session.open) return NET_BAD;
    int result = pump();
    if (result) return result;
    if (session.ready) return 1;
    unsigned int state = br_ssl_engine_current_state(&session.client.eng);
    if (state & BR_SSL_CLOSED) return fail(TLS_PROTOCOL);
    if (!(state & (BR_SSL_SENDAPP | BR_SSL_RECVAPP))) return NET_AGAIN;
    session.ready = 1;
    return 1;
}

int tls_send(const void* data, uint32_t length) {
    br_ssl_engine_context* engine = &session.client.eng;
    size_t room;
    if (!data || !length) return NET_BAD;
    int result = tls_handshake();
    if (result != 1) return result;
    unsigned int state = br_ssl_engine_current_state(engine);
    if (state & BR_SSL_CLOSED) return TLS_PROTOCOL;
    if (!(state & BR_SSL_SENDAPP)) return NET_AGAIN;
    unsigned char* buffer = br_ssl_engine_sendapp_buf(engine, &room);
    uint32_t n = room < length ? (uint32_t)room : length;
    memcpy(buffer, data, n);
    br_ssl_engine_sendapp_ack(engine, n);
    br_ssl_engine_flush(engine, 0);
    result = pump();
    return result ? result : (int)n;
}

int tls_receive(void* data, uint32_t capacity) {
    br_ssl_engine_context* engine = &session.client.eng;
    size_t available;
    if (!data || !capacity) return NET_BAD;
    int result = tls_handshake();
    if (result != 1) return result;
    unsigned int state = br_ssl_engine_current_state(engine);
    if (state & BR_SSL_RECVAPP) {
        unsigned char* buffer = br_ssl_engine_recvapp_buf(engine, &available);
        uint32_t n = available < capacity ? (uint32_t)available : capacity;
        memcpy(data, buffer, n);
        br_ssl_engine_recvapp_ack(engine, n);
        (void)pump();  /* A failure is reported by the next call. */
        return (int)n;
    }
    return state & BR_SSL_CLOSED ? 0 : NET_AGAIN;
}

void tls_close(void) {
    if (!session.open) return;
    br_ssl_engine_context* engine = &session.client.eng;
    if (session.ready && !session.error && !(br_ssl_engine_current_state(engine) & BR_SSL_CLOSED)) {
        br_ssl_engine_close(engine);
        (void)pump();
    }
    erase(&session, sizeof(session));
}

const char* tls_error_text(int error) {
    switch (error) {
    case TLS_NO_TRUST: return "No trusted certificates built in";
    case TLS_NO_RANDOM: return "No hardware random source for TLS";
    case TLS_CERTIFICATE: return "Server certificate rejected";
    case TLS_PROTOCOL: return "TLS handshake or record failure";
    case TLS_BUSY: return "A TLS session is already open";
    case NET_BAD: return "Invalid request";
    case NET_UNAVAILABLE: return "Network unavailable";
    case NET_OFFLINE: return "Not connected";
    case NET_AGAIN: return "Try again";
    case NET_TIMEOUT: return "Timed out";
    case NET_NO_SPACE: return "No free sockets";
    case NET_BAD_HANDLE: return "Invalid socket";
    case NET_REFUSED: return "Connection refused";
    case NET_RESET: return "Connection reset";
    case NET_NOT_FOUND: return "Host not found";
    case NET_AUTH: return "Authentication rejected";
    case NET_IO: return "Network I/O error";
    case NET_BUSY: return "Busy; try again";
    case NET_UNREACHABLE: return "Host unreachable";
    case NET_UNSUPPORTED: return "Not supported";
    default: return "Unknown error";
    }
}
