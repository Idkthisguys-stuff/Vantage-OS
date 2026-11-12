# TLS client

EL0 programs can open TLS 1.2 sessions over a connected TCP socket with the
API in `include/vantage/tls.h`. The client is `user/lib/tls.c`, built on an
unmodified copy of [BearSSL](../third_party/bearssl/README.md) (MIT license,
upstream commit `7bea48e5`). It is linked into the terminal image; the kernel
only supplies sockets and entropy.

**Physical Pi acceptance is pending.** The client is tested on host builds
against the virtual network's HTTPS server and linked into the AArch64 image,
but no handshake with a real server has run on the Pi yet.

## Using it

Connect a TCP socket, then hand it to `tls_open` with the server's name. Every
call returns immediately; `NET_AGAIN` means try again after `sys_frame_wait`.

```c
int64_t handle = sys_net_open(NET_TCP);
/* ... sys_net_connect(handle, address, 443) until it returns 0 ... */
int result = tls_open((uint64_t)handle, "example.com");
while (result == 0 && (result = tls_handshake()) == NET_AGAIN) sys_frame_wait();
if (result == 1) {
    static const char request[] = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    for (uint32_t sent = 0; sent < sizeof(request) - 1;) {
        int n = tls_send(request + sent, sizeof(request) - 1 - sent);
        if (n == NET_AGAIN) { sys_frame_wait(); continue; }
        if (n < 0) break;
        sent += (uint32_t)n;
    }
    static unsigned char data[1024];
    while ((result = tls_receive(data, sizeof(data))) != 0) {
        if (result == NET_AGAIN) { sys_frame_wait(); continue; }
        if (result < 0) break;  /* tls_error_text(result) */
        /* use result bytes */
    }
}
tls_close();
sys_net_close((uint64_t)handle);
```

- One session per process: a second `tls_open` returns `TLS_BUSY` until
  `tls_close`.
- `tls_receive` returns 0 only after the server's `close_notify`. A TCP end of
  stream without it is a truncation and returns `TLS_PROTOCOL`.
- `tls_close` sends `close_notify` when the session is established and the
  socket accepts it, then erases all session state (keys, buffers, the
  certificate context). It never closes the socket.
- After an error, every call returns the same error until `tls_close`.

| Result | Meaning |
|---|---|
| `TLS_NO_TRUST` (-100) | The image was built without trust anchors |
| `TLS_NO_RANDOM` (-101) | `sys_net_random` has no hardware entropy source |
| `TLS_CERTIFICATE` (-102) | Chain not anchored, bad signature, name mismatch, wrong key usage |
| `TLS_PROTOCOL` (-103) | Handshake or record failure, alert, truncation, no common suite |
| `TLS_BUSY` (-104) | A session is already open |
| `NET_*` | The socket's error, unchanged |

## What is supported

- TLS 1.2 only (no 1.0/1.1, no 1.3), client side, with SNI. Renegotiation is
  refused.
- Cipher suites, in the client's preference order:
  `ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256`,
  `ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256`,
  `ECDHE_ECDSA_WITH_AES_128_GCM_SHA256`, `ECDHE_RSA_WITH_AES_128_GCM_SHA256`,
  `ECDHE_ECDSA_WITH_AES_256_GCM_SHA384`, `ECDHE_RSA_WITH_AES_256_GCM_SHA384`.
  ChaCha20 comes first because the Pi's AES runs in constant-time software.
- Key exchange on X25519, P-256, P-384 and P-521.
- Server signatures and certificate chains: RSA PKCS#1 v1.5 (2048 to 4096-bit
  keys) and ECDSA on P-256, P-384 and P-521, with SHA-256, SHA-384 or SHA-512.
- Path validation by BearSSL's `x509_minimal`: the chain must be sent in order
  and reach a certificate issued by a built-in anchor (by DN and signature);
  Basic Constraints and Key Usage are enforced, unknown critical extensions
  are rejected. The host name must match a SAN `dNSName` (or the CN when the
  certificate has no SAN), case-insensitively, with a single leading `*.`
  wildcard.

Not supported: certificate validity dates (below), revocation (CRL/OCSP),
name constraints, client certificates, session resumption, ALPN, TLS 1.3,
CBC/CCM/3DES/static-RSA suites, and SHA-1 signatures. Certificate DNs are
compared byte for byte.

### Validity dates are not checked yet

The clock callback accepts every certificate's validity period, so expired
or not-yet-valid certificates are accepted. The kernel's SNTP time
(`sys_net_time`) is deliberately not used yet: an attacker who can answer the
unauthenticated SNTP query could move the clock. A future change can pass the
synchronized time to `br_x509_minimal_set_time` once its trust is settled.

## Entropy

`tls_open` reads 32 bytes from `sys_net_random` (the BCM2711 RNG200 on the
Pi, the operating system's generator in the simulator) and seeds BearSSL's
HMAC-DRBG with them. Without a hardware source the call fails with
`TLS_NO_RANDOM`; BearSSL's own OS and CPU entropy sources are compiled out,
and the timer-seeded fallback used for network identifiers is never used for
TLS.

## Trust anchors

The Pi build embeds the Mozilla CA certificate store as published by curl,
converted to BearSSL trust anchors at build time:

- `tools/fetch_ca.py` downloads `https://curl.se/ca/cacert-2026-11-05.pem`,
  checks its pinned SHA-256, caches it in `out/ca-bundle/` and writes
  `out/ca-bundle/tls-trust.c` with a JSON manifest. It is regenerated only
  when the bundle or the script changes. All 118 certificates of this bundle
  convert (RSA and EC P-256/P-384/P-521 keys); others would be skipped and
  listed in the manifest.
- `python tools/build.py` does this automatically for kernel builds. If the
  download fails, it warns and builds without anchors: HTTPS then fails with
  `TLS_NO_TRUST`, everything else works.
- `--tls-trust-c FILE` (or `TLS_TRUST_C=FILE` for `make`) uses another
  generated file; `python tools/fetch_ca.py --pem my.pem --output my.c`
  converts your own bundle. `--tls-trust-c none` builds without anchors, as
  does `make` without `TLS_TRUST_C` (`user/lib/tls_trust_none.c`).
- To update, pick the newest dated file on <https://curl.se/docs/caextract.html>,
  set `BUNDLE` and `SHA256` in `tools/fetch_ca.py` (check the hash against the
  published `.sha256` file), and rebuild.

The certificates are Mozilla's data, licensed under the
[Mozilla Public License 2.0](https://www.mozilla.org/MPL/2.0/). They are
downloaded at build time and are not committed. Mozilla's extra per-CA
restrictions (distrust dates, name constraints) are not part of the PEM file
and are not applied.

In the simulator and host tests the built-in anchor is instead the virtual
network's CA. Tests can replace it with `tls_set_trust_anchors` from
`user/lib/tls_trust.h`, which exists only in `VANTAGE_SIM` builds.

## Memory and size

| Item | Size |
|---|---|
| Session state (`.bss`: record buffers, engine, X.509 context) | 40,104 bytes |
| Terminal image without TLS (text and constants) | 36,864 bytes |
| With the TLS client and BearSSL | 114,688 bytes |
| With the 118 trust anchors | 176,128 bytes |

The record buffers are BearSSL's full-duplex size (16,709 bytes in, 16,469
out), so records of any legal size are accepted. Nothing is allocated at run
time. The deepest call chains (RSA-4096 and ECDHE arithmetic during the
handshake) use roughly 6 to 7 KiB of the 16 KiB EL0 stack, estimated from
GCC's per-function stack usage. The kernel image file keeps its size because
`.data` starts at 2 MiB; text and constants below it now leave about 586 KiB
free.

## BearSSL configuration

BearSSL is compiled only through these definitions, in the `Makefile` for
the Pi and in `cmake/VantageTLS.cmake` for host builds:

| Setting | Why |
|---|---|
| `BR_USE_URANDOM=0`, `BR_USE_GETENTROPY=0`, `BR_USE_WIN32_RAND=0`, `BR_RDRAND=0` | Entropy comes only from `sys_net_random` |
| `BR_USE_UNIX_TIME=0`, `BR_USE_WIN32_TIME=0` | No system clock; see validity dates |
| `BR_AES_X86NI=0`, `BR_SSE2=0`, `BR_POWER8=0` | Host tests run the same portable code as the Pi |
| `BR_64=1`, `BR_INT128=1` (Pi) | 64-bit limbs and 64x64 multiplies on the Cortex-A72 |

The Pi build compiles only the BearSSL files the client uses
(`BEARSSL_SOURCES` in `mk/sources.mk`) with the kernel's freestanding flags.
`user/lib/libc/` provides the few C headers BearSSL includes, and
`lib/memory.c` the `memmove` and `strlen` it calls. Host builds compile all of
BearSSL into a static library, so the linker keeps only what is used; the
server code there serves the virtual network.

## The virtual network's HTTPS server

`sim/platform/tls_peer.c` runs a BearSSL server for TCP port 443 on
`vantage.test` (192.168.50.10) and `remote.vantage.test` (203.0.113.10),
serving the same pages as port 80. At first use it derives an EC P-256 CA key
and server key from a fixed-seed HMAC-DRBG and encodes a self-signed CA
certificate and a `vantage.test` certificate (SAN and CN) signed with
ECDSA-SHA256; servers send both. No key or certificate is stored in the
repository. Both hosts present the `vantage.test` certificate, so connecting
to `remote.vantage.test` by that name fails the name check. Test options
(`tls_peer_options`) select an RSA-2048 server key, a single cipher suite,
small records, or a response that ends without `close_notify`.

## Verification

```sh
python tools/build.py test --build-dir out/host     # includes tls_client
ctest --test-dir out/host -R tls_client --output-on-failure
python -m unittest discover -s tests/tools -p test_fetch_ca.py -v
```

`tls_client` drives `user/lib/tls.c` through the real user-library wrappers
and kernel socket syscalls over the production network stack against the
virtual network. It checks an HTTPS GET byte for byte, the name mismatch, no
anchors, wrong anchors (unknown CA name, wrong CA key), missing entropy, a
second open, `close_notify`, truncation, all six suites with EC and RSA server
keys, a suite the client does not offer, and a 20 KB response in about 40
records, also under loss, duplication, reordering and latency.
`test_fetch_ca.py` converts synthetic certificates (no key pairs exist for
them) and checks the download pinning, caching, regeneration, the offline
fallback in `tools/build.py`, and, when an AArch64 compiler is present, that
the generated C compiles against `user/lib/tls_trust.h`.

## Physical acceptance checklist

Pending. Record the commit, kernel SHA-256, the CA bundle date and the serial
log.

- [ ] Boot logs `Hardware RNG ready`; `tls_open` succeeds.
- [ ] HTTPS GET from a public server with an RSA certificate and one with an
      ECDSA certificate; the bodies match the server's content.
- [ ] A server whose certificate does not name the host fails with
      `TLS_CERTIFICATE`.
- [ ] A 100 KB HTTPS download completes; note the time it takes.
