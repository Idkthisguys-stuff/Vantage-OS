# Pi 4 GPU and Wi-Fi drivers

The V3D 4.2 renderer is built in and probed at every boot; a probe that fails
for any reason selects the CPU renderer. CYW43455 Wi-Fi support is enabled by
default but radio initialization is skipped until assets are supplied.
An isolated Pi 4 diagnostic has passed the V3D pixel self-test and direct stop,
followed by CPU terminal display flips. Ethernet controller initialization has
also completed without a cable. Accelerated terminal rendering, Ethernet packet
transfer and Wi-Fi operation still need physical validation. Native models and
linked ARM tests do not establish physical cache coherence. The IPv4 stack,
sockets and terminal commands built on the radio are described in
[NETWORK.md](NETWORK.md).

## Builds

Use a fresh external `BUILD_DIR` after source-layout changes. Build-option
changes are tracked automatically. The existing GNU AArch64 toolchain builds
all driver code in a normal kernel:

```sh
make BUILD_DIR=/path/to/build              # V3D probed at boot, CPU fallback
make BUILD_DIR=/path/to/cpu-build V3D=0    # CPU renderer only
```

`V3D=0` removes the boot-time probe: the image never allocates GPU buffers and
never touches the V3D, bridge or reset registers. The driver code stays linked
in both images.

Wired Ethernet is probed at boot by default. `ETHERNET=0` (or
`python tools/build.py --ethernet 0`) removes the probe: the image never
touches the GENET registers and attaches no Ethernet link. The driver code
stays linked.

For Wi-Fi, the build needs CYW43455 firmware, Pi 4 Model B NVRAM text and CLM
regulatory data. They are not bundled. A normal kernel build downloads the files
Raspberry Pi OS uses on a Pi 4 Model B and embeds them:

```sh
python tools/build.py
```

`tools/fetch_wifi.py` downloads the files from Raspberry Pi's
`firmware-nonfree` repository at a pinned commit, rejects any whose SHA-256
differs from the pinned hash, and caches them in `out/wifi-firmware/`. The files
are covered by the Cypress firmware license; building with them accepts it. Its
text is saved as `out/wifi-firmware/LICENSE-cypress.txt`. Use `--wifi 0` to
build without Wi-Fi.

The radio's legal channels and transmit power depend on the country where the
Pi is used. On the first build, the helper asks
`www.cloudflare.com/cdn-cgi/trace` (or `ipinfo.io` if that fails) for the
country of the network's public IP address and remembers it in
`out/wifi-firmware/country`. A VPN or proxy can report the wrong country, and
the Pi may be used elsewhere: pass `--wifi-country XX` or set
`VANTAGE_WIFI_COUNTRY` to choose it, and `--wifi-country auto` to look it up
again. If the lookup or a download fails, the build continues without Wi-Fi;
with an explicit country it fails instead. `--wifi-regrev` selects a regulatory
revision; the default, -1, uses the firmware's default for the country. Later
builds reuse the cache and regenerate nothing.

To use other files, choose the country and revision explicitly and embed them
yourself:

```sh
python tools/embed_wifi.py --firmware /path/to/brcmfmac43455-sdio.bin \
  --nvram /path/to/board-matched-nvram.txt --clm /path/to/brcmfmac43455-sdio.clm_blob \
  --country GB --regrev 0 --output /path/to/generated/wifi-assets.c
make BUILD_DIR=/path/to/wifi-build WIFI=1 WIFI_FIRMWARE_C=/path/to/generated/wifi-assets.c
```

The country/revision above are an example, not defaults. `WIFI=0` disables
initialization. Without `WIFI_FIRMWARE_C`, the build succeeds and UART reports
the unavailable assets before touching the radio. `WIFI=1` can be
combined with either V3D setting. The embedding tool records asset hashes
beside its generated source. Keep generated firmware source, manifests and
binaries out of Git; board NVRAM can contain device-specific data. Retain the
applicable vendor licenses with distributed images. No network credentials are
included or logged.

## Ownership and failures

All APIs are kernel-only and require core 0, masked EL1 execution and the existing
identity MMU mapping. Zero-initialize driver contexts and retain them for kernel
lifetime. Enabling a driver asserts exclusive ownership of that controller;
firmware/another OS must not concurrently submit work. Calls do not yield.
Secondary CPU workers must complete before GPU ownership transfers.

`drivers/hw/` implements a serialized, bounded property-mailbox transaction and
cache maintenance. Firmware responses are checked for tag, length and completion.
A submitted request that times out quarantines its DMA buffer until reboot.
Other property users must remain synchronous on core 0, including the display's
existing mailbox calls. Polling waits use unsigned timer subtraction and an
iteration bound if the timer stops. APIs return `HwResult` errors.

The display uses a separate, cache-line-isolated mailbox buffer. A timeout
before submission permits a later retry; a submitted timeout quarantines that
buffer until reboot and freezes scanout updates because the flip may still
occur. An invalid flip response also freezes updates. The UART reports the
freeze once; display syscalls return and terminal processing can continue.
The shared FIFO is not a general message router: synchronous callers discard
nonmatching replies, including late replies from a quarantined optional-property
request. No other mailbox consumer may concurrently expect those replies.
Initial physical/virtual geometry and offset responses must acknowledge the
requested triple-page layout before any page is treated as hidden.

## V3D 4.2 rendering backend

The isolated Pi 4 diagnostic passed the clear/present/readback self-test.
Normal accelerated terminal rendering and recovery still need physical
validation; see the acceptance checklist and the
open hardware questions below. No performance or hardware-correctness claim is
made. This is a 2D clear/present path, not an OpenGL/Vulkan implementation or
a shader compiler.

### Backend seam and operations

`include/vantage/kernel/render.h` is the one interface between the display
layer and its renderers. `kernel/ui/render.c` owns the active draw surface and
always has the CPU raster path; an accelerated `RenderBackend` supplies a
surface plus `fill` and `present` and answers each request with done, skip
(CPU raster does it on the same surface), failed, discard-surface or lost-target.
The last two results keep potentially device-owned memory out of CPU writes.
`platform/pi4/display.c`
applies the panel rotation and calls `render_pixel`, `render_fill` and
`render_present`. `platform/pi4/startup.c` probes V3D after the display and
worker cores are up and attaches the backend only after its self-test. The
simulator keeps its own CPU display backend.

| Operation | V3D backend | CPU backend |
|---|---|---|
| Full-surface fill (canvas clear) | One V3D clear job | Four-way `raster_fill_rows` |
| Present to the hidden scanout page | One V3D load/store job, if eligible | Four-way `raster_copy_rows` |
| Partial fills and pixels (lines, circles, text, buttons) | CPU raster on the V3D surface, between jobs | CPU raster |

A present runs on V3D only when the self-test proved it, the firmware pitch is
exactly `width * 4`, and the page is 4 KiB aligned, physically contiguous and
below `0xfc000000`. Otherwise the CPU copies from the V3D surface. Both
renderers store the same 32-bit pixel values unchanged: the clear colour is
the pixel word, and load and store use one RGBA8 raster format. Tests compare
both paths pixel for pixel through the device model.

### Bring-up sequence

`v3d_init` and `v3d_self_test` run these steps in order. Each failure records
its stage and `HwResult`, attempts to stop the device and returns.

| Stage | Check | Typical failure |
|---|---|---|
| (arguments) | Core 0, masked EL1, aligned contiguous buffers below `0xfc000000`, frame a whole number of pages. No hardware access. | `HW_INVALID` |
| power domain | Property `0x00038030`, domain 11 on. The observed Pi 4 response is status 0 and state 1, not an echoed domain ID. | mailbox error, `HW_PROTOCOL` |
| clock | Property `0x00038001`, clock 5 on: echoed clock ID, state bit 0 set and bit 1 clear; then `0x00030002` must report a nonzero rate. | mailbox error, `HW_PROTOCOL` |
| reset bridge | Stop both AXI bridges, assert `PM_V3DRSTN`, wait, release, restart the master then the slave bridge. Both restarts must be acknowledged. | `HW_TIMEOUT` |
| identity | Core `IDENT0` reads "V3D" version 4 and `IDENT1` revision 2; hub `IDENT1` reads 4.2 with one core; hub `IDENT2` reports an MMU. | `HW_UNSUPPORTED` |
| idle check | No compute dispatch and no TFU job pending after reset. | `HW_BUSY` |
| MMU setup | Zero the table, map the arena ranges, mask and clear interrupts, program the table base, abort-on-fault control and scratch page, flush the TLB. Table base and enable bit are read back; no fault bit may be set. | `HW_TIMEOUT`, `HW_IO` |
| clear job / present job | Self-test jobs: clear, present to the hidden scanout page, clear. | `HW_TIMEOUT`, `HW_IO` |
| pixel verification | Every cleared pixel, every presented pixel (surface and scanout page) and the guard page after the surface are read back. | `HW_IO` |

Every wait is bounded: 100 ms per register wait, 500 ms per job phase, 1 s per
property call, each with an iteration bound in case the timer stops. The first
bridge stop of the reset cycle may go unacknowledged on a bridge that never
ran; only the restart must succeed.

Stopping the device (after any failure, or `v3d_stop`): mask interrupts,
request a GMP stop and wait for outstanding AXI transfers, stop the slave and
master bridges, assert `PM_V3DRSTN` and read it back. The firmware domain stays
powered for the kernel lifetime: requesting domain-off after the direct stop
timed out on the test Pi and subsequent framebuffer requests also timed out.
Keeping the domain on allowed the next diagnostic to complete its first
application frame and later display flips. This is a stop, not a power-saving
transition. Steps that address the core are skipped if bring-up never reached
it. Mapped buffers are released only when both bridges acknowledged and the
reset bit reads back asserted; an unconfirmed stop still quarantines them.

Boot logs one of:

```text
[OK] V3D self-test passed; V3D backend selected (clear, present)
[OK] V3D self-test passed; V3D backend selected (clear; CPU present)
[ERR] V3D unavailable: <stage> (<HwResult>)
[INFO] CPU rendering backend selected
[INFO] V3D disabled at build time (V3D=0); CPU rendering backend selected
```

### Memory ownership

| Buffer | Allocated by | GPU mapping | Who writes, when | Cache maintenance |
|---|---|---|---|---|
| GPU page table, 4 MiB, 64 KiB aligned | `startup.c`, page allocator | None; its address is the MMU table base | Driver only: at init, and the present-window entries around a present job | Cleaned after every write, then a TLB flush, before any job |
| Arena control lists (bin, render, tile; 4 KiB each) | `startup.c`, one 2 MiB page run, 4 KiB aligned | `0x01000000`, permanent | Driver builds them before a submit; nobody during a job | Cleaned before submit |
| Arena tile allocation (64 KiB) and tile state (128 KiB) | same run | permanent | GPU only | Arena cleaned and invalidated once at init |
| Arena draw surface (up to 1.5 MiB) and guard page | same run | permanent | CPU raster while the device is idle; GPU during a clear job | Clear: clean+invalidate before, invalidate after. Present: clean before |
| Arena MMU scratch page (last page) | same run | Never mapped; target of faulting accesses | Nobody | None |
| Scanout page (present target) | VideoCore firmware; `display.c` picks the hidden page | `0x01400000`, then unmapped and the TLB flushed; retained on unconfirmed stop | GPU during the job; CPU only after ownership returns | Clean+invalidate before, invalidate after (the CPU mapping is non-cached) |
| CPU back buffer | `fb_init`, kernel heap | Never | CPU; standby while V3D draws | None |

The gaps between arena ranges and the rest of the 4 GiB GPU space stay
unmapped, so the device cannot reach the kernel, user memory, its own page
table or the visible scanout pages. The CPU uses an identity mapping, so the
physical address equals the pointer; V3D is on the SCB bus with identity DMA
addresses, not the VC4 `0xc0000000` alias (the display masks that alias off
the firmware framebuffer address). A page-table entry is the page frame number
plus valid and writeable bits. The CPU page tables are unchanged.

Lifetime: the table and arena are lent to the driver for one bring-up. When
the driver ends in `V3D_STOPPED` (the table address was never programmed, or
the stop was confirmed) `startup.c` frees both runs. When it ends in
`V3D_QUARANTINED` (stop unconfirmed) both stay reserved until reboot and the
UART says so. Any external target retained in `gpu.target` is also retired
until reboot: the display never writes or flips to that page again. This
covers a failed boot self-test as well as a runtime present. The other two
scanout pages provide CPU double buffering. Target ownership is retained
before mapping starts and returns only after a successful unmap/TLB flush or
a confirmed stop; a failed flush cannot establish ownership.
A failed allocation frees the other run and selects the CPU
renderer. The CPU back buffer is never freed, so a runtime fallback cannot
fail for lack of memory. A context is never restarted.

### Synchronization and failure handling

- Every display operation runs on core 0 at EL1 with all DAIF masks set, from
  entry to return. The engine checks this before each job and the renderer
  halts on re-entry. Jobs normally finish before return. An unconfirmed stop
  may leave work in flight, so its table, arena and any exposed target remain
  reserved until reboot.
- CPU raster batches (`parallel_run`) join all workers before returning, and
  no job is submitted from inside a batch. Workers and the device therefore
  never write the surface at the same time. After an unconfirmed clear, the
  CPU abandons that surface without reading it and refills its standby buffer.
  A failed present only reads the surface: the renderer may copy its intact
  contents to the CPU buffer, but never modifies the quarantined source.
- Sections that must not be preempted or re-entered once timer preemption
  exists: each `render_pixel`/`render_fill`/`render_present` call (including
  the V3D job, its cache/TLB maintenance and any raster batch), `fb_swap`
  through the page flip, and each property-mailbox transaction. If a future
  scheduler runs display syscalls with interrupts unmasked, the engine
  refuses the job (`HW_INVALID`) and the operation runs on the CPU; it must
  keep these sections masked or add its own serialization.
- Completion is polled: the 8-bit bin and render frame counters, compared
  for change so wrap is harmless. The V3D interrupt line is not used and all
  interrupts stay masked. Each poll also checks the hub MMU interrupt bits,
  the MMU control fault bits and the core out-of-memory and GMP-violation
  bits. `V3D_ERR_STAT` is logged after a failure but never decides one.
- No GPU retry. The first timeout or fault attempts the stop described above
  and the backend stays off until reboot. A failed clear is redone over every
  CPU-buffer pixel; an unconfirmed clear discards the GPU surface rather than
  reading it. A failed present preserves the intact source in the CPU buffer.
  A confirmed stop permits retrying the same hidden page. An unconfirmed stop
  returns a lost-target result without a CPU copy to that page: the display
  retires it permanently and presents the frame to another safe hidden page.
  The same retirement applies if the boot self-test loses its target before
  backend attachment. Late writes from the abandoned job can therefore reach
  only reserved memory. The UART logs
  `[ERR] V3D stopped during <stage> (<HwResult>)` and
  `[INFO] CPU rendering backend selected; current frame kept`.

### Physical acceptance checklist

Pending. Keep each result tied to the board revision, firmware version,
commit, build options, kernel SHA-256 and the full UART log:

- [ ] Repeated cold boots reach `V3D backend selected (clear, present)`; note
      any boot that falls back and its stage and code.
- [ ] Warm reboots after a previous V3D session bring the device up again.
- [ ] `V3D=0` image boots, logs the build-time message and renders correctly.
- [ ] Injected failures in disposable builds (wrong expected identity, a job
      list that never completes, a corrupted self-test compare, a withheld
      bridge acknowledgement) each log the expected stage, fall back to the
      CPU and keep the UI usable. A failure injected after boot keeps the
      current frame.
- [ ] An unconfirmed present stop retires its scanout page, including during
      the boot self-test. Late GPU writes cannot corrupt subsequent CPU frames.
- [ ] Pixel comparison: capture scanout after clears, mixed CPU shapes and
      presents on the V3D image and the `V3D=0` image; they must be identical,
      including colour order and the alpha byte.
- [ ] Sustained mixed CPU/GPU rendering with touch input shows no stale or
      torn regions (CPU cache and device cache transitions).
- [ ] Frame times for clear and present on both images, recorded before any
      performance statement.
- [ ] After a runtime failure, the freed 6 MiB is reused without corruption.

### Open hardware questions

These could not be confirmed from the sources and are handled conservatively
(a failing check selects the CPU renderer):

- The isolated diagnostic confirmed domain-on `{0, 1}`, clock enable at
  500 MHz, identity, MMU setup and the pixel self-test on board revision
  `0x00B03115` with firmware `0x6AE892C8`. The domain-retained diagnostic image
  SHA-256 was `9fb5449b95ff19afaee1d7d0b79ff361e6a451baeed2dfc1ef2b827085f18d0e`.
  Its terminal remained CPU-rendered; physical visibility and input, normal
  accelerated terminal rendering and combined-device operation are not proven
  by that UART log.
- Whether the first bridge stop on a cold domain is acknowledged, and whether
  cycling `PM_V3DRSTN` conflicts with the firmware's own domain management.
- Whether V3D can store into the firmware-allocated framebuffer. The boot
  self-test reads the page back; a mismatch disables the whole backend.
- Whether stores clip to a frame that is not a multiple of the 64-pixel tile
  size. The guard page after the surface is checked by the self-test.
- Whether masked interrupts still latch in the status registers. Completion
  does not depend on them and MMU faults are also read from `MMU_CTL`.
- Which `V3D_ERR_STAT` bits are status rather than errors; the register is
  only logged.
- Real cache behaviour of the CPU/GPU hand-over and the cost of the per-job
  TLB flushes.

## CYW43455 Wi-Fi

The supported target is **chip 0x4345, revision 6**, as used on Pi 4 Model B.
Other chip revisions fail explicitly. Firmware must support WPA2 supplicant
offload and the requested control variables; unsupported firmware is an error.

`drivers/wifi/sdio.c` uses the legacy EMMC controller at `0xfe300000` and GPIO
34–39. It queries the source clock, powers WLAN through firmware GPIO 129 and
uses polling CMD52/CMD53 PIO, four-bit bus width and at most 25 MHz. It does not
modify GPIO 48–53, SD slot power/voltage or the EMMC2 controller at `0xfe340000`.

The radio driver discovers backplane cores and CR4 RAM, halts the radio CPU,
packs NVRAM, uploads and reads back firmware/NVRAM, writes the reset vector and
starts firmware. It loads CLM data, checks regulatory status, configures the
explicit country and obtains the MAC address without printing it.

SDPCM/BCDC transport implements control request matching, flow credits, sequence
wrap, checked frame lengths, event decoding and four bounded receive slots.
Malformed framing stops the transport. Glom aggregation is disabled; receiving
an unsupported aggregation channel stops rather than misinterpreting it.

`pi4_wifi_device()` returns the initialized kernel device; after a successful
boot, `platform/pi4/net.c` attaches it to the network service as its link.
Boot keeps the radio powered while unassociated (`mpc` 0), selects active scans
and subscribes to association, link, key-exchange, deauthentication and escan
events.

`wifi_join_start` accepts an explicit 1–32-byte SSID with either an 8–63-byte
ASCII WPA2-PSK passphrase or, explicitly, no passphrase for an open network. For
WPA2 it sets CCMP/RSN parameters and requests firmware key offload; for open
networks it disables encryption and WPA authentication. It drains old
notifications, requests the SSID and returns without waiting. `wifi_poll` then
completes the join only after association, link and (WPA2) key-exchange events,
and a read-back of the associated SSID; deauthentication or disassociation
during the join fails it, and the 30 s limit times it out. `wifi_join_status`
reports busy, connected, not associated or the failure. A failed WPA2 join never
falls back to an open network. `wifi_join` remains as a blocking WPA2 wrapper.
Host-side key request buffers and the pending SSID are scrubbed. Frame transport
stays unauthorized until the join has been verified. `wifi_send`/`wifi_receive`
transport Ethernet frames through the firmware BDC header.

`wifi_scan_start` requests an escan (version 1 parameters: all channels, any BSS
type, firmware dwell defaults). Each partial result must carry exactly one
`bss_info` record whose length, SSID length and information-element range are
consistent with the event; others are ignored. Hidden SSIDs are skipped. RSN and
WPA elements classify security as open, WEP, WPA, WPA2, WPA3, WPA2/WPA3,
802.1X or other; a truncated element list is "other". Results merge by BSSID,
keep the strongest 16, and complete on the final event (or abort status), or
after 15 s. `wifi_rssi` reads the connected signal strength.

There is no WPA3 SAE, 802.1X, roaming, power-saving policy or AP mode.

## BCM54213 Ethernet

Wired Ethernet uses the BCM2711 GENET v5 MAC (`drivers/ethernet/genet.c`) and
the board's BCM54213PE PHY on the UniMAC MDIO bus, connected over RGMII.
**Physical Pi 4 validation is pending.** The register models and linked ARM
tests below are software checks; they prove neither link establishment, RGMII
timing nor DMA coherence on hardware.

### Ownership and polling

The ownership rules above apply: core 0, masked EL1, the identity MMU mapping
(GENET at `0xfd580000` lies in the existing Device-memory peripheral blocks).
`platform/pi4/ethernet.c` owns the only `Genet` context, a static 96 KiB
object holding all DMA buffers, and exposes it to the network service as the
Ethernet `NetLink`. The context stays reserved until reboot because its
buffers remain programmed into the DMA rings. Nothing uses interrupts: GENET's
interrupt controllers are masked and the network service drives the device
through `poll`, `send`, `receive` and `carrier`. No call allocates or waits for
a link.

### Bring-up

`genet_init` runs once from `pi4_drivers_start`, after Wi-Fi:

1. The revision register must report GENET v5. Otherwise nothing is written and
   boot continues without Ethernet.
2. The MAC address comes from the firmware property tag `0x00010003`. A zero or
   multicast address fails bring-up. The firmware answers with a 6-byte value
   in an 8-byte buffer; the property layer accepts responses shorter than the
   request buffer. A failed query or invalid address leaves the controller
   untouched apart from the revision read.
3. The UniMAC is soft-reset with local loopback before DMA accesses, releasing
   its boot-time reset as in U-Boot and Circle. The receive buffer is reset,
   counters are cleared, the maximum frame is 1536 bytes, the receive status
   block is off and each
   received frame is preceded by two alignment bytes. All GENET interrupts are
   masked. Any DMA a previous owner left running is then stopped and must
   report stopped within 10 ms before its rings and buffers are replaced.
   Each required reset/flush delay must complete; a stopped timer aborts
   initialization without proceeding to descriptor setup. A partial reset
   failure does not attempt DMA-register cleanup.
4. The station address is programmed and the destination filter passes only
   broadcast and the station address (no multicast, not promiscuous). The
   hardware filter block is cleared.
5. The port is external-GPHY RGMII with the MAC's internal transmit delay
   disabled, as in U-Boot and Circle; the PHY's reset defaults provide the
   clock delays.
6. The PHY at MDIO address 1 must identify as `0x600d84aX`. It is reset (at
   most 500 ms), advertises 10/100 Mbit/s half and full duplex and 1000 Mbit/s
   half and full duplex without pause, and auto-negotiation is restarted.
7. Only the default queue (ring 16) is used: 32 receive and 16 transmit
   descriptors with 2 KiB buffers. The receive buffers are cleaned and
   invalidated before receive DMA starts; then DMA and UniMAC transmit and
   receive are enabled.

Every MDIO transaction is bounded to 10 ms. Boot logs one of:

```text
[OK] Ethernet ready (MAC xx:xx:xx:xx:xx:xx)
[INFO] Ethernet unavailable (no GENET v5 controller)
[INFO] Ethernet unavailable (<HwResult>)
[INFO] Ethernet disabled at build time (ETHERNET=0)
```

The ready line contains the board's MAC address; remove it from logs before
sharing them publicly.

### DMA

GENET is a 40-bit-capable SCB bus master. In the Pi 4 firmware device tree the
`scb` bus maps `0x7d580000` to `0xfd580000` and its `dma-ranges` map RAM one to
one, so descriptors take ARM physical addresses, split into LO/HI words. The
descriptors themselves are GENET registers. To transmit, the frame is copied
into a free transmit buffer, padded with zeros to 60 bytes, cleaned from the
cache, described (start and end of packet, hardware CRC) and published through
the producer index. A received frame is found by comparing the hardware
producer index with the consumer index; its descriptor status is checked, the
received bytes are invalidated and copied out, and the consumer index returns
the buffer. The CPU never writes a receive buffer. The 16-bit indices wrap; if
the hardware-owned index does not reset, the driver adopts its value.

### Link handling

`genet_poll` reclaims completed transmissions, reports waiting frames and reads
the PHY status at most every 100 ms. The latched-low link bit is read twice
while the link is down, so a returned cable is seen in one check and a short
drop is seen as a drop. On link up the negotiated mode is resolved from both
sides' abilities (1000, then 100, then 10 Mbit/s; full before half duplex; a
master/slave fault counts as no link). The UniMAC speed and half-duplex bits
and the RGMII link bit are then programmed; pause frames are ignored. On link
down the RGMII link bit is cleared. `genet_carrier` returns the last result
without MDIO traffic. While the link is down, sends fail with `NET_OFFLINE`;
frames already queued are left to the hardware.

Receive frames without both start and end of packet, with an error flag
(overrun, CRC, receive error, odd nibble, too long) or shorter than 14 or
longer than 1514 bytes are dropped and counted. A full transmit ring returns
`NET_BUSY`. `poll` reports the device lost (and it stays off until reboot, with
DMA and the UniMAC stopped) after three consecutive failed PHY checks, after
1 s of transmit without progress while the link is up, or when a hardware index
claims more entries than the ring holds.

### Validation

`tests/native/drivers/genet.c` (`genet_ethernet`) models the GENET registers,
ring 16, a BCM54213PE behind MDIO, the mailbox property and a CPU cache whose
lines reach the device only when cleaned and return only when invalidated. It
covers an absent or older controller, refused ownership, MAC failures,
register and filter programming, PHY identity, reset timeout and
auto-negotiation, link up and down at every speed and duplex, latched link
drops, rate-limited MDIO, transmit padding and completion reclaim, a full
transmit ring, receive error drops, ring and 16-bit index wrap, device
discards, stuck MDIO and DMA, a transmit stall and inconsistent hardware
indices. `tests/arm/ethernet_boot.py` runs the linked kernel's
`pi4_drivers_start` against an MMIO model: the controller comes up without a
cable and reports the MAC, then links, sends and receives through identity DMA
addresses; a stuck MDIO bus is reported and boot continues; an `ETHERNET=0`
image never touches the controller. `tests/arm/v3d_boot.py` and
`tests/arm/wifi_boot.py` boot with GENET absent.

```sh
ctest --test-dir /path/to/driver-tests -R genet_ethernet --output-on-failure
python tests/arm/ethernet_boot.py /path/to/v3d0-no-assets/kernel8.elf /path/to/v3d0-ethernet0/kernel8.elf -v
```

Physical acceptance is pending. Keep each result tied to the board revision,
firmware version, commit, build options and the UART log (MAC removed):

- [ ] Boot without a cable reaches the UI with `[OK] Ethernet ready`, and
      without delay.
- [ ] Link up at 1000, 100 and 10 Mbit/s (forced on the switch side) with the
      reported speed; full and half duplex where the switch allows it.
- [ ] Cable unplug and replug is detected within a second each way.
- [ ] DHCP, ARP, ping and TCP over Ethernet against a controlled peer, with
      sustained bidirectional traffic and no corrupted or lost frames at idle
      load.
- [ ] Boot after a network boot (bootloader DMA left active) still brings the
      device up cleanly.
- [ ] `ETHERNET=0` image boots and logs the build-time message.

References: [U-Boot `drivers/net/bcmgenet.c`](https://github.com/u-boot/u-boot/blob/master/drivers/net/bcmgenet.c)
(GPL-2.0+) and [Circle `lib/bcm54213.cpp`](https://github.com/rsta2/circle/blob/master/lib/bcm54213.cpp)
(GPL-3.0) for registers and bring-up order;
[Linux v6.12 GENET](https://github.com/torvalds/linux/tree/v6.12/drivers/net/ethernet/broadcom/genet)
for behaviour only; the Pi 4 firmware device tree for the address, DMA ranges
and PHY address.

## Verification

```sh
cmake -S . -B /path/to/driver-tests
cmake --build /path/to/driver-tests
ctest --test-dir /path/to/driver-tests -L drivers --output-on-failure
python -m unittest discover -s tests/tools -v
python tests/arm/pi4_drivers.py /path/to/fresh/kernel8.elf -v
python tests/arm/v3d_boot.py /path/to/default/kernel8.elf /path/to/v3d0/kernel8.elf -v
python tests/arm/display_mailbox.py /path/to/fresh/kernel8.elf -v
python tests/arm/wifi_boot.py /path/to/v3d0-no-assets/kernel8.elf /path/to/wifi0-v3d0/kernel8.elf -v
```

The native models decode GPU command packets and model SDHCI, backplane RAM,
firmware responses and wireless events. Tests use synthetic data, not vendor
firmware. They cover successful paths, bounds, ownership, counter/timer wrap,
timeouts, malformed frames, authentication rejection, receive overflow, escan
records and security classes, open and non-blocking joins, deauthentication
during a join and RSSI.
They do not model GPU microarchitecture, RF, firmware internals or physical DMA.

The V3D model (`tests/support/v3d_model.c`) sits behind `HwOps`. It models
the firmware properties, reset line, bridges, identity and MMU registers, and
executes the generated control lists tile by tile with frame clipping. The
device reads a RAM shadow that changes only when the driver cleans the CPU
cache, the CPU sees device output only after it invalidates, and device
addresses go through the TLB snapshot of the last flush, so a missing cache
or TLB step shows up as wrong pixels or a failed check. A job stays in flight
for several polls; touching its buffers, the cache or other registers in that
window fails the test. `v3d_commands_and_mmu` covers successful bring-up,
failure at every bring-up stage, job timeout and fault, quarantine and the
exact mapped page set. `render_backends` runs the production renderer, adapter
and engine against CPU rendering after every operation, including failures
injected mid-session. `tests/arm/v3d_boot.py` boots the linked default and
`V3D=0` images through the real boot coordinator with MMIO-level models of a
working, absent and failing device and checks the selected backend, the
presented frame and the memory returned to the allocator.
It also injects a present that outlives failed GMP/bridge/reset stops, completes
the abandoned write after CPU fallback, and checks that neither CPU writes nor
scanout rotation reuse the retired page. A wedged clear checks that the CPU
never reads the abandoned surface. Modified or unacknowledged framebuffer
geometry is rejected before allocation or device startup.
`tests/arm/display_mailbox.py` executes the actual display mailbox transport
against scripted MMIO, including pre-submit retries, submitted-timeout
quarantine, invalid flip acknowledgements, late optional-property replies and
a stopped timer. It checks that uncertain flips freeze subsequent scanout
writes and request-buffer reuse.

For hardware acceptance, retain commit/image/asset hashes, board and firmware
versions and a serial log. Never include credentials or device MACs in public
logs. Validate GPU self-test, color order and sustained mixed CPU/GPU rendering;
compare pixels with the CPU path. Verify bounded failure and CPU fallback in a
disposable build. Separately validate SDIO discovery, firmware readiness, WPA2
join/rejection, link loss and bidirectional Ethernet traffic with a controlled
test peer. Network-level checks are listed in [NETWORK.md](NETWORK.md#physical-acceptance-checklist).

## References and licenses

- [Pi 4 bare-metal V3D examples](https://github.com/Random06457/rpi4-gpu-bare-metal-examples/tree/a4f74ff75963cf387fe5a42b6c6aeb2d67923c5f): packet layouts and initialization; [MIT notice](licenses/v3d-examples-MIT.txt).
- [Linux v6.12 V3D](https://github.com/torvalds/linux/tree/v6.12/drivers/gpu/drm/v3d): MMU/cache/register semantics, GPL-2.0-or-later portions used under GPL version 3.
- [Linux v6.12 BCM2835 power domains](https://github.com/torvalds/linux/blob/v6.12/drivers/pmdomain/bcm/bcm2835-power.c): V3D reset line and AXI bridge stop/start order, GPL-2.0-or-later portions used under GPL version 3.
- [Mesa 24.0 Broadcom packet definitions](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-24.0.0/src/broadcom/cle/v3d_packet.xml): V3D 4.2 control-list field layouts used to check the generated packets and the test decoders; [MIT notice](licenses/mesa-broadcom-cle-MIT.txt).
- [Firmware mailbox property interface](https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface) and Linux v6.12 `include/soc/bcm2835/raspberrypi-firmware.h`: power-domain and clock property tags.
- [Linux v6.12 brcmfmac](https://github.com/torvalds/linux/tree/v6.12/drivers/net/wireless/broadcom/brcm80211/brcmfmac): CYW43455, SDPCM/BCDC and WPA firmware protocol; [ISC notice](licenses/brcmfmac-ISC.txt).
- [BCM2711 device tree](https://github.com/torvalds/linux/blob/v6.12/arch/arm/boot/dts/broadcom/bcm2711.dtsi) and [BCM2711 peripherals](https://datasheets.raspberrypi.com/bcm2711/bcm2711-peripherals.pdf).
