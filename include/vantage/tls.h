#pragma once
#include <vantage/base.h>

/* TLS 1.2 client for EL0 programs, layered on a connected TCP socket from
 * sys_net_open/sys_net_connect. One session per process at a time. Calls
 * never block: NET_AGAIN means call again after sys_frame_wait. Other
 * negative results are NetError values from the socket or a TlsError below.
 * The server's certificate chain must lead to a built-in trust anchor and
 * name `host`. Certificate validity dates are not checked yet. */
enum TlsError {
    TLS_NO_TRUST = -100,     /* No trust anchors were built in. */
    TLS_NO_RANDOM = -101,    /* No hardware entropy source. */
    TLS_CERTIFICATE = -102,  /* Server certificate rejected. */
    TLS_PROTOCOL = -103,     /* Handshake or record-layer failure. */
    TLS_BUSY = -104          /* A session is already open. */
};

/* Starts a session; 0 or an error. host is the server name (SNI and
 * certificate name check), at most NET_NAME_MAX - 1 characters. */
int tls_open(uint64_t socket, const char* host);
/* 1 when the handshake is complete, NET_AGAIN while it runs, or an error. */
int tls_handshake(void);
/* Bytes accepted (at least 1), NET_AGAIN, or an error. */
int tls_send(const void* data, uint32_t length);
/* Bytes received, 0 once the server closed the session, NET_AGAIN, or an error. */
int tls_receive(void* data, uint32_t capacity);
/* Sends close_notify if possible and ends the session. The socket stays open. */
void tls_close(void);
/* Short description of a TlsError or NetError. */
const char* tls_error_text(int error);
