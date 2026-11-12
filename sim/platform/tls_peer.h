#pragma once
#include <vantage/base.h>

/* HTTPS for the virtual network (net_peer.h): one BearSSL server session per
 * web-host TCP connection on port 443. A CA and a vantage.test certificate are
 * generated at first use from a fixed seed, so no key material is stored in
 * the repository. remote.vantage.test presents the same certificate, so its
 * name does not match. The CA is the simulator's built-in trust anchor
 * (tls_sim_trust_anchors in user/lib/tls_trust.h). Test infrastructure only. */
#define TLS_PEER_SLOTS 4U

typedef struct {
    uint32_t record_bytes;  /* Largest application record sent; 0 = 16384. */
    int truncate;           /* End responses with TCP FIN but no close_notify. */
    int rsa;                /* Present an RSA-2048 key instead of EC P-256. */
    uint16_t suite;         /* The only cipher suite accepted; 0 = BearSSL's full list. */
} TlsPeerOptions;

/* Receives records for the client. */
typedef void TlsPeerSink(void* context, const unsigned char* data, unsigned int bytes);

TlsPeerOptions* tls_peer_options(void);
/* Starts a server session for connection `slot` (< TLS_PEER_SLOTS). */
void tls_peer_accept(unsigned int slot);
/* Feeds bytes from the client. Decrypted application data is copied to plain
 * (anything beyond capacity is discarded); returns its length, or -1 once the
 * session has failed or closed. */
int tls_peer_input(unsigned int slot, const unsigned char* data, unsigned int bytes,
                   unsigned char* plain, unsigned int capacity, TlsPeerSink* sink, void* context);
/* Encrypts application data. */
void tls_peer_write(unsigned int slot, const unsigned char* data, unsigned int bytes,
                    TlsPeerSink* sink, void* context);
/* Ends the session: close_notify unless truncate is set. */
void tls_peer_finish(unsigned int slot, TlsPeerSink* sink, void* context);
