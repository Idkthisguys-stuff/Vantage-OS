# Pi 4 hardware roadmap

The opt-in USB driver brings up the Pi 4 VL805 controller, checks a No Op
command completion, and polls USB 2 boot-protocol keyboards and mice through
the VL805's internal USB 2 hub. Keyboard presses go to Home and Console. The
mouse moves a cursor and its left button acts as a touch contact. One keyboard
and one mouse can be used together. The keyboard uses a US layout.

The same host controller can read one USB 2 Mass Storage Bulk-Only drive.
Files displays up to 256 regular files from the FAT32 root via their short
names. Open a file for a read-only text preview and use Next to read through it.
The filesystem accepts a FAT32 volume or a primary MBR FAT32 partition with
512-byte sectors. A device removal invalidates the list and any open preview.

Physical Pi validation is pending. The driver supports one high-speed USB 2
hub tier to reach devices plugged directly into the Pi. Cascaded hubs, general
HID report descriptors, subdirectories, long filenames and writes are later steps.

## USB mass storage

1. **Validate the controller and USB input on the Pi.** The driver trains the
   BCM2711 PCIe link, brings up the VL805 firmware, and enables the command and
   event rings in DMA-visible memory. The probe-only entry point still leaves
   bus mastering disabled. The USB-enabled image verifies a No Op completion
   before scanning ports. Cold boot from microSD and test input and reconnect.
2. **Exercise ring and reset recovery.** Test completion across ring wrap,
   unplug during a transfer, failed commands, and safe DMA ownership through
   recovery. Compare the physical results with the controller model.
3. **Validate bulk transfers and FAT32 on the Pi.** The host model exercises
   endpoint configuration, a Bulk-Only status stall and recovery, capacity
   discovery and a known FAT32 file read. On the Pi, open the same file as on a
   PC, advance across a sector boundary, and unplug and reconnect the drive.
4. **Extend directory and filename handling.** Add subdirectories and long
   filenames to the read-only Files view.
5. **Enable writes and safe eject.** Add FAT allocation and metadata updates,
   verify flushing and error recovery, and test reconnects and interrupted
   writes on expendable media. Enable writes in Files after data survives
   eject and remount on both the Pi and a PC.

## First USB board run

Build a Pi 4 image with USB enabled:

```sh
python tools/build.py --usb 1 --wifi 0 --v3d 0 --ethernet 0 --tls-trust-c none --build-dir out/pi4-usb
```

Use `out/pi4-usb/prod/kernel8.img` with the existing microSD boot setup and
capture UART. Expect `[OK] VL805 xHCI running` with the version and port count.
Plug a wired boot-protocol keyboard and mouse directly into the USB-A ports.
Press `1` to open Console, type `help` and press Enter, then use the mouse to
click Home and another tile. Check reconnect and unplug while holding the
left button. Plug in a FAT32 stick with a short-name text file in the root,
then open **Files → USB** and select it. On failure, record the UART stage and
detail, board revision,
bootloader version and device models. The USB controller is disabled in
ordinary builds (`USB=0`).

## Simulator

Set the optional `VANTAGE_USB_IMAGE` environment variable to an existing
regular FAT32 disk image to test **Files → USB** in the Windows simulator.
The simulator opens the image read-only.

`VANTAGE_USB_IMAGE` can be set independently of `VANTAGE_STORAGE_IMAGE`.

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

## Bluetooth and Rust

Validate [Bluetooth startup and BLE discovery](BLUETOOTH.md) with Wi-Fi and
USB disabled. On the first board run, scan for a known BLE device and
compare its reported address or name. After isolated runs pass, check
Bluetooth scanning while Wi-Fi remains associated.

The C xHCI rings provide a baseline for a future allocation-free Rust
component. Give each pending command a typed slot and validate completions
before reusing slots. Keep MMIO, cache maintenance and DMA address translation
at the hardware boundary. Packet decoders and FAT32 are later candidates
with clear buffer and lifetime boundaries.

Register and firmware references are the [BCM2711 device tree](https://github.com/torvalds/linux/blob/v6.12/arch/arm/boot/dts/broadcom/bcm2711.dtsi),
[Broadcom PCIe host driver](https://github.com/torvalds/linux/blob/v6.12/drivers/pci/controller/pcie-brcmstb.c),
and [Pi firmware notes on the VL805 and inbound DMA](https://github.com/raspberrypi/firmware/issues/1495#issuecomment-728112060).
The [Intel xHCI specification](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf),
[USB 2.0 specification](https://www.usb.org/document-library/usb-20-specification)
and [USB-IF HID specification](https://www.usb.org/sites/default/files/hid1_11.pdf)
define the controller rings, USB 2 hub behavior and boot reports. The
[USB-IF Bulk-Only Transport specification](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf)
defines the mass storage wire protocol. See
[Raspberry Pi USB hardware documentation](https://www.raspberrypi.com/documentation/computers/raspberry-pi.html#usb)
for the Pi 4's USB-A routing.
