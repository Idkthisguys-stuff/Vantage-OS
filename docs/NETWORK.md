# Wi-Fi and IPv4 networking

Vantage joins WPA2-PSK or explicitly open 802.11 networks with the Pi 4's
CYW43455 radio, configures IPv4 with DHCP, resolves names, and gives processes
UDP, ICMP echo and TCP client sockets. The terminal exposes this through the
`net` command.

**Physical Pi acceptance is pending.** The stack is validated on host tests
against an independent virtual network, on the CYW43455 protocol model, and on
linked ARM models of the syscall boundary. None of these exercise RF, real
firmware, SDIO timing or real access points. The radio also needs licensed
firmware, NVRAM and CLM files at build time, which `python tools/build.py`
downloads and embeds, accepting their license. Without them the kernel reports
Wi-Fi as unavailable. See
[the hardware guide](HARDWARE.md#builds).

## Using it

```text
> net scan
Scanning... (Cancel to stop)
  Signal  Security    Network
  strong  WPA2        "VantageNet"
> net join VantageNet
Passphrase: ************
Joining "VantageNet"... (Cancel to stop)
Connected. Address 192.168.50.100. Saved; joins automatically at boot.
> net
Connected to Wi-Fi "VantageNet" (WPA2, strong signal)
Address: 192.168.50.100/24
Router: 192.168.50.1  DNS: 192.168.50.1
Time: 2026-11-11 09:41:07 UTC
> net ping vantage.test 3
> net lookup www.vantage.test
> net get http://vantage.test/
> net leave
```

| Command | Action |
|---|---|
| `net` | Connection (Ethernet and its speed, or Wi-Fi network and signal), address, router, DNS, network time and the last error |
| `net scan` | Nearby Wi-Fi networks, strongest first, at most 16 |
| `net join NAME [--open] [--hidden]` | Join Wi-Fi. Asks for the WPA2 passphrase (8–63 characters) unless `--open`; `--hidden` finds a network that does not broadcast its name. Once connected the network is saved |
| `net leave` | Disconnect Wi-Fi; saved networks are not joined again until the next `net join` |
| `net saved` | List saved networks, marked `(hidden)` or `(open)`; passphrases are never shown |
| `net forget NAME` | Remove a saved network |
| `net ping HOST [COUNT]` | ICMP echo, once per second, 1–100 requests (default 4) |
| `net lookup HOST` | Find a host's IPv4 address |
| `net get URL [-o FILE]` | HTTP/1.0 GET of an `http://` or `https://` URL; prints the status line and body (up to 32 KiB), or saves the body to FILE |
| `net help` | List these commands |

Options may come before or after the name; quote names that contain spaces:
`net join "Home Network" --hidden`. The passphrase is echoed as `*` on screen
and over serial, removed from memory after use, and never printed. Network
commands run across frames: the display stays live and Cancel (Ctrl+C) stops
them. Cancel during a join disconnects.

`net get` shows a redirect's `Location`. With `-o FILE` the body is saved only
for a successful (2xx) answer, relative to the current directory, replacing an
existing file; if the transfer fails, is cancelled or the file reaches its
capacity, the partial file is removed. `https://` URLs use the built-in TLS
client (default port 443).

Saved networks are stored in `/ram/.netprofiles` (at most 8) and handed to the
kernel when the terminal starts, which joins them automatically. **Passphrases
are stored there in plain text** (hex-encoded); anyone with the storage device
can read them. Without mounted storage a network is saved for the current
session only. The file is a header line `vantage-networks 1` followed by one
line per network: `0` or `1` (hidden), the name and the passphrase as lowercase
hex, `-` for an open network. Malformed lines are ignored.

In the desktop simulator, `net` uses a built-in virtual network rather than the
host computer's network. It offers `VantageNet` (WPA2, passphrase
`vantage-demo`), `Vantage Guest` (open) and `Neighbour WPA3` (WPA3-only, which
this stack refuses). Its router is 192.168.50.1; `vantage.test` (192.168.50.10)
and `remote.vantage.test` (203.0.113.10, behind the router) serve HTTP on port
80, echo on 7, discard on 9 and a byte generator on 19.

## Architecture

```text
user/apps/terminal/net_*.c     the net command, saved-network file, HTTP(S)
user/lib/tls.c                 TLS client (BearSSL), see TLS.md
user/lib/user_lib.c            sys_net_* wrappers (syscalls 48..56)
kernel/net/net_syscall.c       copy-in/copy-out validation per calling process
kernel/net/service.c           links, reconnect and auto-connect, status, scan, join
kernel/net/profile.c           saved networks (kernel memory)
kernel/net/sntp.c              network time
kernel/net/socket.c            process-owned UDP/ICMP/TCP sockets
kernel/net/tcp.c               TCP client state machines
kernel/net/dhcp.c, dns.c       DHCPv4 client, stub resolver and cache
kernel/net/ip.c                Ethernet II, ARP, IPv4, ICMP, UDP
        NetLink (include/vantage/kernel/net.h)
platform/pi4/net.c             CYW43455 as NetLink; RNG200 entropy
platform/pi4/ethernet.c        GENET as NetLink
drivers/wifi/cyw43455.c        scan, join, keys, frames (SDPCM/BCDC)
sim/platform/net_sim.c         simulator station on the virtual network
sim/platform/net_peer.c        virtual APs, router, DHCP/DNS servers, TCP hosts
```

The network service is single-owner: all of it runs on core 0 with interrupts
masked, like the file, display and input services, and only immutable
core-0-pinned callers may use it. Nothing blocks. `net_poll` services the radio
(up to 32 SDPCM packets), delivers received frames and runs the DHCP, ARP, DNS
and TCP timers. It runs on every frame wait and presentation, and at the start
of every network syscall. A process that polls slowly delays protocol timers
accordingly; nothing runs while no process makes these calls.

Time is a 64-bit millisecond counter extended from the 32-bit microsecond
timer, so the service must be polled at least once every 71 minutes.

### Link management

`JOIN` validates and copies the request, then starts association and returns.
Progress is reported by `STATUS` (`joining`, `up`, `down`, `retrying`). The
CYW43455 driver now joins without waiting: `wifi_join_start` configures security
and requests association, and `wifi_poll` completes the join only after the
association, link and (for WPA2) key-exchange events, followed by an SSID
read-back. Deauthentication or disassociation during the join fails it.

- A first join that fails reports the error (for example
  `Authentication or association rejected`) and erases the credentials.
- Once a network has been joined, a lost link withdraws the address, resets TCP
  connections and retries with the same credentials after 2, 4, 8 … up to 60 s,
  until `net leave`. Credentials are kept in kernel memory only for this.
- `net leave` releases the DHCP lease, disassociates, erases credentials and
  pauses automatic joining until the next join.
- A radio failure stops the link for the rest of the boot (`available = 0`).

Supported: WPA2-PSK (CCMP, firmware supplicant offload) and open networks, also
WPA2 on WPA2/WPA3 transition networks. Not supported: WPA3-only (SAE), WPA/TKIP,
WEP, 802.1X, roaming, power saving and AP mode. Hidden networks are joined by
name with `NET_JOIN_HIDDEN` (a directed probe); a missing network reports
`NET_NOT_FOUND`.

### Saved networks

Up to 8 profiles live in kernel memory (`SYS_NET_PROFILE`); the terminal keeps
them in `/ram/.netprofiles` and loads them at startup. While Wi-Fi is down and
not paused, the kernel scans and joins the strongest visible saved network,
then hidden ones by probe, then the next candidate; after all fail it waits
30 s, doubling up to 5 minutes. A saved network that drops is retried 3 times
before the kernel picks again. An explicit join always wins. The file stores
passphrases as plain text.

### Ethernet

When an Ethernet link is attached, IPv4 uses it while the cable is up (checked
every 500 ms) and Wi-Fi otherwise. Switching withdraws the address, resets TCP
connections, takes the new interface's MAC and runs DHCP there. Wi-Fi stays
associated in the background. See HARDWARE.md for the GENET driver.

### Network time

Once an address is bound, the kernel asks the DHCP-supplied NTP servers
(option 42), or `pool.ntp.org`, for the time (SNTPv4: 2 s wait, 3 tries,
backoff from 15 s to 15 minutes, hourly resync). `SYS_NET_TIME` reports it and
`net` shows it. Nothing else uses it yet; in particular TLS does not check
certificate dates.

### IPv4

- **Ethernet/ARP:** 8 neighbour entries, refreshed after 5 minutes. The newest
  frame to an unresolved neighbour waits for resolution (3 requests, 1 s apart);
  a neighbour that does not answer is unreachable for 3 s. ARP updates only
  existing entries, except that a request for our address adds its sender.
- **IPv4:** no fragmentation or reassembly (fragments are dropped, DF is set),
  received options are ignored, no multicast. MTU 1500.
- **ICMP:** answers echo requests; echo replies go to ICMP sockets; destination
  unreachable fails a TCP connection attempt or reports a UDP error.
  Closed UDP ports answer with port unreachable.
- **DHCP (RFC 2131):** discover with randomized backoff (4 s to 64 s), request,
  renew at T1 by unicast, rebind at T2 by broadcast, retry at half the remaining
  time (at least 60 s), release on leave. A NAK or expiry restarts discovery.
  Leases shorter than 60 s are treated as 60 s. No ARP conflict probe is made
  before using the address.
- **DNS:** A records over UDP from the DHCP-supplied servers (up to two),
  4 attempts with 1/2/3/4 s timeouts. Replies must come from a configured
  server's port 53 to the query's random port, with its random ID and question.
  Compression is followed with a 16-jump limit, CNAME chains up to 8. Results are
  cached for the TTL, held between 5 s and 1 h; failures are held for 5 s.
  Dotted IPv4 literals need no network.

### TCP

Client connections only (no listening sockets). The connection sends an MSS
option of 1460 and honours the peer's; it uses RFC 6298 retransmission timing
(1 s to 60 s) with Karn's rule, slow start and congestion avoidance with fast
retransmit after three duplicate ACKs, zero-window probing (probes never abort
a connection), and RFC 5961 checks before accepting a reset or SYN.

| Limit | Value |
|---|---|
| Connection attempt | SYN retried 5 times: fails after about 63 s |
| Unacknowledged data | 8 retransmissions, then reset with `NET_TIMEOUT` |
| Buffers per connection | 4 KiB send, 8 KiB receive (window ≤ 8 KiB) |
| TIME-WAIT | 30 s; the oldest is reused when all sockets are in use |
| Closed but unfinished | Released after at most 120 s |

Out-of-order segments are dropped and acknowledged, so a single loss costs a
retransmission. There is no window scaling, SACK, timestamps, delayed ACK,
Nagle, keepalive or urgent data. Closing a socket sends FIN after queued data;
closing with unread received data, or with `NET_CLOSE_ABORT`, sends a reset.

## System call interface

Numbers 48–53 are additive. Payloads are fixed-size and copied in full; output
buffers are checked for write access before any effect. Inaccessible buffers
terminate the process; misalignment and invalid fields return `NET_BAD`. The
file and task calls are unchanged.

| ID | Call | Payload | Result |
|---:|---|---|---|
| 48 | `SYS_NET_STATUS` | `NetStatus` (120 bytes, out) | 0 |
| 49 | `SYS_NET_SCAN` | `NetScan` (64 bytes) | START: 0; RESULT: 1 entry, 0 end, `NET_AGAIN` scanning |
| 50 | `SYS_NET_JOIN` | `NetJoin` (104 bytes, in; erased after the call) | 0 when started |
| 51 | `SYS_NET_LEAVE` | none | 0 |
| 52 | `SYS_NET_RESOLVE` | `NetResolve` (264 bytes) | 1 with address, `NET_AGAIN` pending |
| 53 | `SYS_NET_SOCKET` | `NetSocketCall` (1064 bytes) | per operation |

Socket operations are `OPEN` (returns a handle), `CONNECT`, `SEND` (bytes
accepted), `RECEIVE` (bytes; 0 is TCP end of stream), `CLOSE` and `STATUS`.
TCP `CONNECT` returns `NET_AGAIN` until the connection is established. UDP sends
to `address`/`port` when given, otherwise to the connected peer. ICMP sockets send
echo requests with the sequence number in `port` and receive matching replies.
Data is at most 1024 bytes per call (UDP datagrams up to 1472 bytes are received
truncated to the call's capacity). Each UDP/ICMP socket queues 4 datagrams.

There are 8 sockets system-wide. Handles carry a generation, so stale handles
are rejected, and belong to the calling process: other processes cannot use
them, and reaping a process closes its sockets. Error values are listed in
`include/vantage/abi/net.h`.

IPv4 addresses in the ABI are host integers, first octet most significant
(`192.168.1.2` is `0xc0a80102`).

## Entropy

Transaction IDs, ephemeral ports and TCP initial sequence numbers come from the
BCM2711 RNG200 (0xfe104000), enabled at boot. A health-test failure is reset
once; data is used only after the status reads clean. If the generator does not
produce data, boot logs a warning and a timer-seeded xorshift generator is used.
That fallback avoids collisions but is predictable.

## Security notes

- Passphrases travel in a fixed payload that the library and kernel erase after
  copying. The kernel keeps them only while joined or reconnecting.
- The terminal does not print MAC addresses or BSSIDs.
- DNS, NTP and plain HTTP are unauthenticated: a host on the network can answer
  queries or rewrite traffic. `https://` uses TLS 1.2 with the Mozilla CA
  bundle (see TLS.md); certificate dates are not checked yet.

## Verification

```sh
python tools/build.py test --build-dir out/host       # includes network_stack
ctest --test-dir out/host -L network --output-on-failure
python tools/build.py sim --sdl /path/to/SDL2 --build-dir out/desktop
ctest --test-dir out/desktop -R terminal_network --output-on-failure
python tests/arm/network.py out/pi4/kernel8.elf -v
```

- `network_stack` runs the production service, IPv4 stack, DHCP, DNS, TCP and
  socket syscalls against `sim/platform/net_peer.c`, an independent
  implementation of the other end. It covers scans and join failures, DHCP
  renew/rebind/NAK/expiry/release, ARP failure, ICMP on- and off-link, DNS
  (CNAME, compression loop, spoofed ID, NXDOMAIN, timeout, TTL), UDP errors,
  HTTP, refused connections, 230 KB of echoed data and a 100 KB download
  checked byte for byte under loss, duplication, reordering and latency, zero
  windows, connection and retransmission timeouts, link loss and reconnection,
  socket ownership and limits, the syscall copy checks, and 20,000 mutated frames.
- `wifi_firmware_and_frames` adds escan record validation and security
  classification, scan merging and limits, open and non-blocking joins,
  deauthentication during a join and RSSI to the CYW43455 model.
- `terminal_model` covers command parsing, the masked and erased passphrase,
  join outcomes and Cancel.
- `terminal_network` drives the real SDL terminal through scan, join, DHCP,
  ping, DNS, HTTP and leave on the virtual network.
- `tests/arm/network.py` runs EL0 programs on the linked kernel: copied payloads,
  socket release when a process is reaped, faults for inaccessible buffers, the
  terminal commands without a radio, and the RNG200 start-up and fallback.

The simulator's virtual network has zero latency by default, so `ping` times
there are 0 ms. Throughput and latency have not been measured on hardware.

## Physical acceptance checklist

Pending. Record board revision, firmware/NVRAM/CLM hashes, commit, kernel
SHA-256 and the full serial log for each item. Do not publish passphrases.

- [ ] Boot logs `Hardware RNG ready` and `CYW43455 firmware ready`.
- [ ] `wifi scan` lists nearby networks with plausible signal, channel and security.
- [ ] WPA2 join succeeds; a wrong passphrase fails without connecting; an open
      network joins only with `-o`.
- [ ] DHCP address, gateway and DNS match the router; renewal is seen in its log.
- [ ] `ping` to the gateway and an internet host; `dns` for public names.
- [ ] `http` fetches a page from a controlled server, including a 100 KB body.
- [ ] Switching the access point off and on: the link retries and recovers, and
      open TCP connections report a reset.
- [ ] Repeated join/leave cycles, and a long idle period across DHCP renewal.
