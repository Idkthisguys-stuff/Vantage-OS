# Pi 4 hardware roadmap

Start the next backend change with xHCI command and event rings. While a Pi is
unavailable, test the linked kernel against a controller model and a known
disk image. When a Pi is available, collect separate USB, Wi-Fi and V3D boot
logs so failures can be traced to one driver.

## USB mass storage

1. **Probe the Pi 4 controller.** The kernel's opt-in probe trains the BCM2711
   PCIe link, identifies the VIA VL805 and reads its xHCI capabilities. UART
   reports the xHCI version and logical port count or the failed stage. PCIe
   bus mastering stays disabled. Native and linked ARM models cover link
   timeout, endpoint discovery and BAR validation. On the Pi, cold boot from
   microSD repeatedly and check for a consistent xHCI version and a working Home
   screen after the probe.
2. **Run an xHCI command to completion.** Set up the controller's DMA window,
   load VL805 firmware where required, and reset xHCI. Initialize the command
   and event rings in DMA-visible memory with explicit cache maintenance.
   Verify a No Op command produces a completion event in the linked ARM
   model. Cover ring wrap, timeouts, and a completed reset before reclaiming
   DMA buffers.
3. **Enumerate one flash drive and read a sector.** Reset a USB-A port, read
   descriptors over the control endpoint, configure the Bulk-Only interface,
   and wire its two bulk endpoints into the existing SCSI transport. Exercise
   capacity discovery and a known sector read in the model. On the Pi, check
   the sector contents against the same drive on a PC and repeat after
   unplugging and reconnecting it.
4. **Open files from Home.** Mount the flash drive read-only and expose it
   through Files. Read a known file on the Pi and compare its contents with
   the PC copy, then confirm Files handles an unplug during a read. The
   initial filesystem code reads short-name files in the FAT32 root on a
   512-byte-sector volume. Extend the directory and filename handling as
   required by the Files view.
5. **Enable writes and safe eject.** Add FAT allocation and metadata updates,
   verify flushing and error recovery, and test reconnects and interrupted
   writes on expendable media. Enable writes in Files after data survives
   eject and remount on both the Pi and a PC.

## First USB board run

For the controller probe, build a Pi 4 image with:

```sh
python tools/build.py --usb 1 --wifi 0 --v3d 0 --ethernet 0 --tls-trust-c none
```

Use the existing microSD boot setup and capture UART. Expect
`[OK] VL805 xHCI PCIe probe` followed by the version and logical port count.
On failure, record the stage and detail along with the board revision and
bootloader version. The probe resets the onboard PCIe USB controller and is
disabled in ordinary builds (`USB=0`).

## Wi-Fi and V3D board runs

- **Wi-Fi:** [PR #25](https://github.com/Idkthisguys-stuff/Vantage-OS/pull/25)
  addresses the GPIO mailbox acknowledgement and changes how Wi-Fi discovers
  the chip's backplane blocks. The previous Pi trace stopped at `chip_probe`.
  Use the CPU renderer and embed the [Wi-Fi assets](NETWORK.md). With Ethernet
  unplugged, confirm firmware startup, scan, join a WPA2 network, obtain a DHCP
  address, resolve a name, and download a known HTTPS file. Compare the
  downloaded file with the source. Capture the failed stage and SDIO trace
  if startup stalls. A repeatable download is the network prerequisite for
  future OTA work.
- **V3D:** [PR #23](https://github.com/Idkthisguys-stuff/Vantage-OS/pull/23)
  fixed the shutdown and CPU fallback path. The Pi diagnostic passed the GPU
  pixel self-test. Test repeated Home and Console redraws with V3D enabled and
  Wi-Fi and USB disabled. Record whether UART reports V3D presentation or
  CPU presentation. Repeat with the CPU-only build as a baseline.

Register and firmware references are the [BCM2711 device tree](https://github.com/torvalds/linux/blob/v6.12/arch/arm/boot/dts/broadcom/bcm2711.dtsi),
[Broadcom PCIe host driver](https://github.com/torvalds/linux/blob/v6.12/drivers/pci/controller/pcie-brcmstb.c),
and [Pi firmware notes on the VL805 and inbound DMA](https://github.com/raspberrypi/firmware/issues/1495#issuecomment-728112060).
