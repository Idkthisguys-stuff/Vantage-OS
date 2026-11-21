# Pi 4 hardware roadmap

## USB mass storage

1. **Probe the Pi 4 controller.** The kernel's opt-in probe trains the BCM2711
   PCIe link, identifies the VIA VL805 and reads its xHCI capabilities. UART
   reports the xHCI version and logical port count or the failed stage. PCIe
   bus mastering stays disabled. Native and linked ARM models cover link
   timeout, endpoint discovery and BAR validation. The next
   check on a Pi is a repeatable cold boot from microSD with the same xHCI
   version reported each time and Home reachable after the probe.
2. **Enumerate one directly connected drive.** Initialize the xHCI command,
   event and transfer rings, maintain cache coherence within the controller's
   DMA address range, and load VL805 firmware on boards that require it. Add
   bounded port reset, descriptor reads, device addressing and endpoint
   configuration. Start with one FAT32 flash drive connected directly to a
   Pi 4 USB-A port. The milestone is
   a reported mass-storage interface and a repeatable sector read across
   unplug and reconnect. The kernel already has model-tested SCSI Bulk-Only
   and read-only FAT32 code that the xHCI transport can call.
3. **Open files from Home.** Mount the flash drive read-only and expose it
   through Files. Read a known file on the Pi and compare its contents with
   the PC copy, then confirm Files handles an unplug during a read. The
   initial filesystem code reads short-name files in the FAT32 root on a
   512-byte-sector volume. Extend the directory and filename handling as
   required by the Files view.
4. **Enable writes and safe eject.** Add FAT allocation and metadata updates,
   verify flushing and error recovery, and test reconnects and interrupted
   writes on expendable media. Enable writes in Files after data survives
   eject and remount on both the Pi and a PC.

For the controller probe, build a Pi 4 image with:

```sh
python tools/build.py --usb 1 --wifi 0 --v3d 0 --ethernet 0 --tls-trust-c none
```

Use the existing microSD boot setup and capture UART. Expect
`[OK] VL805 xHCI PCIe probe` followed by the version and logical port count.
On failure, record the stage and detail along with the board revision and
bootloader version. The probe resets the onboard PCIe USB controller and is
disabled in ordinary builds (`USB=0`).

## Wi-Fi and V3D

- **Wi-Fi:** [PR #25](https://github.com/Idkthisguys-stuff/Vantage-OS/pull/25)
  addresses the GPIO mailbox acknowledgement and changes how Wi-Fi discovers
  the chip's backplane blocks. The previous Pi trace stopped at `chip_probe`.
  First confirm firmware startup on a Pi with the Wi-Fi assets embedded. Then
  verify WPA2 association, DHCP, DNS and a file download over Wi-Fi with
  Ethernet unplugged. Use the failed stage and SDIO trace if startup stalls.
- **V3D:** [PR #23](https://github.com/Idkthisguys-stuff/Vantage-OS/pull/23)
  fixed the shutdown and CPU fallback path. The Pi diagnostic passed the GPU
  pixel self-test. Test normal Home and Console across repeated redraws with
  V3D enabled, verify which presentation path UART reports, and check CPU
  fallback in a separate run.

Register and firmware references are the [BCM2711 device tree](https://github.com/torvalds/linux/blob/v6.12/arch/arm/boot/dts/broadcom/bcm2711.dtsi),
[Broadcom PCIe host driver](https://github.com/torvalds/linux/blob/v6.12/drivers/pci/controller/pcie-brcmstb.c),
and [Pi firmware notes on the VL805 and inbound DMA](https://github.com/raspberrypi/firmware/issues/1495#issuecomment-728112060).
