/* BearSSL servers for the virtual network's HTTPS hosts, and the test PKI:
 * keys from a fixed-seed HMAC-DRBG and minimal X.509 v3 certificates signed
 * with ECDSA-SHA256, built at first use. */
#include "tls_peer.h"
#include "user/lib/tls_trust.h"
#include <string.h>

typedef struct {
    int used, done;
    br_ssl_server_context server;
    unsigned char buffer[BR_SSL_BUFSIZE_BIDI];
} Slot;

static TlsPeerOptions options;
static Slot slots[TLS_PEER_SLOTS];

static struct {
    int ready, rsa_ready;
    br_hmac_drbg_context rng;
    br_ec_private_key ca_key, ec_key;
    br_ec_public_key ca_public, ec_public;
    br_rsa_private_key rsa_key;
    br_rsa_public_key rsa_public;
    unsigned char ca_secret[BR_EC_KBUF_PRIV_MAX_SIZE], ca_point[BR_EC_KBUF_PUB_MAX_SIZE];
    unsigned char ec_secret[BR_EC_KBUF_PRIV_MAX_SIZE], ec_point[BR_EC_KBUF_PUB_MAX_SIZE];
    unsigned char rsa_secret[BR_RSA_KBUF_PRIV_SIZE(2048)], rsa_numbers[BR_RSA_KBUF_PUB_SIZE(2048)];
    unsigned char ca_name[64], ca_der[1024], ec_der[1024], rsa_der[1536];
    br_x509_certificate ec_chain[2], rsa_chain[2];
    br_x509_trust_anchor anchor;
} pki;

/* DER writer: end() inserts the minimal length encoding for its begin(). */
typedef struct {
    unsigned char* data;
    size_t length, capacity, open[8];
    unsigned int depth;
} Der;

static Der der(unsigned char* data, size_t capacity) {
    Der d;
    memset(&d, 0, sizeof(d));
    d.data = data;
    d.capacity = capacity;
    return d;
}

static void raw(Der* d, const void* bytes, size_t n) {
    if (d->length + n <= d->capacity) memcpy(d->data + d->length, bytes, n);
    d->length += n;
}

static void begin(Der* d, unsigned char tag) {
    raw(d, &tag, 1);
    d->open[d->depth++] = d->length;
}

static void end(Der* d) {
    size_t start = d->open[--d->depth], n = d->length - start, head = n < 128 ? 1 : n < 256 ? 2 : 3;
    if (d->length + head > d->capacity) { d->length += head; return; }
    memmove(d->data + start + head, d->data + start, n);
    if (head == 1) d->data[start] = (unsigned char)n;
    else if (head == 2) { d->data[start] = 0x81; d->data[start + 1] = (unsigned char)n; }
    else { d->data[start] = 0x82; d->data[start + 1] = (unsigned char)(n >> 8); d->data[start + 2] = (unsigned char)n; }
    d->length += head;
}

static void primitive(Der* d, unsigned char tag, const void* bytes, size_t n) {
    begin(d, tag);
    raw(d, bytes, n);
    end(d);
}

static void integer(Der* d, const unsigned char* bytes, size_t n) {
    while (n > 1 && !bytes[0]) { bytes++; n--; }
    begin(d, 0x02);
    if (bytes[0] & 0x80) raw(d, "", 1);
    raw(d, bytes, n);
    end(d);
}

static const unsigned char ecdsa_sha256[] = {0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02};

static void name(Der* d, const char* common_name) {
    static const unsigned char cn[] = {0x55, 0x04, 0x03};
    begin(d, 0x30); begin(d, 0x31); begin(d, 0x30);
    primitive(d, 0x06, cn, sizeof(cn));
    primitive(d, 0x0c, common_name, strlen(common_name));
    end(d); end(d); end(d);
}

static void extension(Der* d, unsigned char oid_last, const void* value, size_t n) {
    unsigned char oid[3] = {0x55, 0x1d, 0};
    oid[2] = oid_last;
    begin(d, 0x30);
    primitive(d, 0x06, oid, sizeof(oid));
    if (oid_last != 0x11) primitive(d, 0x01, "\xff", 1);  /* Critical, except subjectAltName. */
    primitive(d, 0x04, value, n);
    end(d);
}

enum { CA_CERTIFICATE, EC_CERTIFICATE, RSA_CERTIFICATE };
#define CA_NAME "Vantage Virtual Network CA"

/* The self-signed CA certificate, or a vantage.test certificate signed by
 * the CA. Servers send both, so clients also walk a two-certificate chain. */
static size_t certificate(unsigned char* out, size_t capacity, int kind) {
    static const unsigned char ec_key[] = {0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
                                           0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07};
    static const unsigned char rsa_key[] = {0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00};
    static const unsigned char version[] = {0x02, 0x01, 0x02}, ca[] = {0x30, 0x03, 0x01, 0x01, 0xff};
    static const unsigned char signing[] = {0x03, 0x02, 0x07, 0x80}, certifying[] = {0x03, 0x02, 0x01, 0x06};
    unsigned char tbs[1280], san[64], hash[32], signature[80], serial = (unsigned char)(kind + 1);
    Der t = der(tbs, sizeof(tbs)), s = der(san, sizeof(san)), c = der(out, capacity);
    br_sha256_context sha;
    const br_ec_public_key* key = kind == CA_CERTIFICATE ? &pki.ca_public : &pki.ec_public;
    begin(&t, 0x30);
    begin(&t, 0xa0); raw(&t, version, sizeof(version)); end(&t);
    integer(&t, &serial, 1);
    raw(&t, ecdsa_sha256, sizeof(ecdsa_sha256));
    name(&t, CA_NAME);
    begin(&t, 0x30);
    primitive(&t, 0x17, "250101000000Z", 13);
    primitive(&t, 0x18, "20991231235959Z", 15);
    end(&t);
    name(&t, kind == CA_CERTIFICATE ? CA_NAME : "vantage.test");
    begin(&t, 0x30);
    begin(&t, 0x30);
    if (kind == RSA_CERTIFICATE) raw(&t, rsa_key, sizeof(rsa_key)); else raw(&t, ec_key, sizeof(ec_key));
    end(&t);
    begin(&t, 0x03);
    raw(&t, "", 1);
    if (kind == RSA_CERTIFICATE) {
        begin(&t, 0x30);
        integer(&t, pki.rsa_public.n, pki.rsa_public.nlen);
        integer(&t, pki.rsa_public.e, pki.rsa_public.elen);
        end(&t);
    } else raw(&t, key->q, key->qlen);
    end(&t);
    end(&t);
    begin(&t, 0xa3); begin(&t, 0x30);
    if (kind == CA_CERTIFICATE) {
        extension(&t, 0x13, ca, sizeof(ca));
        extension(&t, 0x0f, certifying, sizeof(certifying));
    } else {
        begin(&s, 0x30); primitive(&s, 0x82, "vantage.test", 12); end(&s);
        extension(&t, 0x0f, signing, sizeof(signing));
        extension(&t, 0x11, san, s.length);
    }
    end(&t); end(&t);
    end(&t);
    if (t.length > t.capacity || s.length > s.capacity) return 0;
    br_sha256_init(&sha);
    br_sha256_update(&sha, tbs, t.length);
    br_sha256_out(&sha, hash);
    size_t signature_length = br_ecdsa_sign_asn1_get_default()(br_ec_get_default(), &br_sha256_vtable, hash,
                                                                &pki.ca_key, signature);
    if (!signature_length) return 0;
    begin(&c, 0x30);
    raw(&c, tbs, t.length);
    raw(&c, ecdsa_sha256, sizeof(ecdsa_sha256));
    begin(&c, 0x03); raw(&c, "", 1); raw(&c, signature, signature_length); end(&c);
    end(&c);
    return c.length <= c.capacity ? c.length : 0;
}

static void setup(void) {
    static const char seed[] = "Vantage virtual network test PKI";
    const br_ec_impl* ec = br_ec_get_default();
    if (pki.ready) return;
    br_hmac_drbg_init(&pki.rng, &br_sha256_vtable, seed, sizeof(seed) - 1);
    br_ec_keygen(&pki.rng.vtable, ec, &pki.ca_key, pki.ca_secret, BR_EC_secp256r1);
    br_ec_compute_pub(ec, &pki.ca_public, pki.ca_point, &pki.ca_key);
    br_ec_keygen(&pki.rng.vtable, ec, &pki.ec_key, pki.ec_secret, BR_EC_secp256r1);
    br_ec_compute_pub(ec, &pki.ec_public, pki.ec_point, &pki.ec_key);
    Der n = der(pki.ca_name, sizeof(pki.ca_name));
    name(&n, CA_NAME);
    pki.anchor.dn.data = pki.ca_name;
    pki.anchor.dn.len = n.length;
    pki.anchor.flags = BR_X509_TA_CA;
    pki.anchor.pkey.key_type = BR_KEYTYPE_EC;
    pki.anchor.pkey.key.ec = pki.ca_public;
    pki.ec_chain[0].data = pki.ec_der;
    pki.ec_chain[0].data_len = certificate(pki.ec_der, sizeof(pki.ec_der), EC_CERTIFICATE);
    pki.ec_chain[1].data = pki.ca_der;
    pki.ec_chain[1].data_len = certificate(pki.ca_der, sizeof(pki.ca_der), CA_CERTIFICATE);
    pki.ready = pki.ec_chain[0].data_len && pki.ec_chain[1].data_len ? 1 : -1;
}

static void setup_rsa(void) {
    setup();
    if (pki.rsa_ready || pki.ready != 1) return;
    pki.rsa_ready = -1;
    if (!br_rsa_keygen_get_default()(&pki.rng.vtable, &pki.rsa_key, pki.rsa_secret, &pki.rsa_public,
                                     pki.rsa_numbers, 2048, 65537)) return;
    pki.rsa_chain[0].data = pki.rsa_der;
    pki.rsa_chain[0].data_len = certificate(pki.rsa_der, sizeof(pki.rsa_der), RSA_CERTIFICATE);
    pki.rsa_chain[1] = pki.ec_chain[1];
    if (pki.rsa_chain[0].data_len) pki.rsa_ready = 1;
}

const br_x509_trust_anchor* tls_sim_trust_anchors(size_t* count) {
    setup();
    *count = pki.ready == 1 ? 1 : 0;
    return &pki.anchor;
}

TlsPeerOptions* tls_peer_options(void) { return &options; }

void tls_peer_accept(unsigned int slot) {
    unsigned char seed[32];
    if (slot >= TLS_PEER_SLOTS) return;
    Slot* s = &slots[slot];
    memset(s, 0, sizeof(*s) - sizeof(s->buffer));
    if (options.rsa) setup_rsa(); else setup();
    if ((options.rsa ? pki.rsa_ready : pki.ready) != 1) return;
    if (options.rsa) br_ssl_server_init_full_rsa(&s->server, pki.rsa_chain, 2, &pki.rsa_key);
    else br_ssl_server_init_full_ec(&s->server, pki.ec_chain, 2, BR_KEYTYPE_EC, &pki.ec_key);
    if (options.suite) br_ssl_engine_set_suites(&s->server.eng, &options.suite, 1);
    br_ssl_engine_set_buffer(&s->server.eng, s->buffer, sizeof(s->buffer), 1);
    pki.rng.vtable->generate(&pki.rng.vtable, seed, sizeof(seed));
    br_ssl_engine_inject_entropy(&s->server.eng, seed, sizeof(seed));
    s->used = br_ssl_server_reset(&s->server);
}

static void drain(Slot* s, TlsPeerSink* sink, void* context) {
    size_t length;
    unsigned char* data;
    while ((data = br_ssl_engine_sendrec_buf(&s->server.eng, &length)) != NULL) {
        sink(context, data, (unsigned int)length);
        br_ssl_engine_sendrec_ack(&s->server.eng, length);
    }
}

int tls_peer_input(unsigned int slot, const unsigned char* data, unsigned int bytes,
                   unsigned char* plain, unsigned int capacity, TlsPeerSink* sink, void* context) {
    if (slot >= TLS_PEER_SLOTS || !slots[slot].used || slots[slot].done) return -1;
    Slot* s = &slots[slot];
    br_ssl_engine_context* engine = &s->server.eng;
    unsigned int got = 0;
    for (;;) {
        size_t length;
        unsigned char* buffer;
        drain(s, sink, context);
        if (br_ssl_engine_current_state(engine) & BR_SSL_CLOSED) { s->done = 1; return -1; }
        if ((buffer = br_ssl_engine_recvapp_buf(engine, &length)) != NULL) {
            size_t take = length < capacity - got ? length : capacity - got;
            memcpy(plain + got, buffer, take);
            got += (unsigned int)take;
            br_ssl_engine_recvapp_ack(engine, length);
        } else if (bytes && (buffer = br_ssl_engine_recvrec_buf(engine, &length)) != NULL) {
            size_t take = length < bytes ? length : bytes;
            memcpy(buffer, data, take);
            br_ssl_engine_recvrec_ack(engine, take);
            data += take;
            bytes -= (unsigned int)take;
        } else return (int)got;
    }
}

void tls_peer_write(unsigned int slot, const unsigned char* data, unsigned int bytes,
                    TlsPeerSink* sink, void* context) {
    if (slot >= TLS_PEER_SLOTS || !slots[slot].used || slots[slot].done) return;
    Slot* s = &slots[slot];
    size_t record = options.record_bytes ? options.record_bytes : 16384U, length;
    unsigned char* buffer;
    while (bytes) {
        drain(s, sink, context);
        if ((buffer = br_ssl_engine_sendapp_buf(&s->server.eng, &length)) == NULL) return;
        if (length > record) length = record;
        if (length > bytes) length = bytes;
        memcpy(buffer, data, length);
        br_ssl_engine_sendapp_ack(&s->server.eng, length);
        br_ssl_engine_flush(&s->server.eng, 0);
        data += length;
        bytes -= (unsigned int)length;
    }
    drain(s, sink, context);
}

void tls_peer_finish(unsigned int slot, TlsPeerSink* sink, void* context) {
    if (slot >= TLS_PEER_SLOTS || !slots[slot].used || slots[slot].done) return;
    if (!options.truncate) br_ssl_engine_close(&slots[slot].server.eng);
    drain(&slots[slot], sink, context);
    slots[slot].done = 1;
}
