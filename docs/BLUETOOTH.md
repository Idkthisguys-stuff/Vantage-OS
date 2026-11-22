# Bluetooth on Raspberry Pi 4 Model B

The Pi 4 Model B has Bluetooth 5.0 and Bluetooth Low Energy. Vantage's
experimental driver uses the onboard radio's UART0 H4 link. The serial
console continues to use the mini-UART on the GPIO header. Pi hardware
references are the [Pi 4 specification](https://www.raspberrypi.com/products/raspberry-pi-4-model-b/specifications/),
[UART mapping](https://www.raspberrypi.com/documentation/computers/configuration.html)
and [CYW43455 datasheet](https://www.infineon.com/assets/row/public/documents/30/49/infineon-cyw43455-datasheet-en.pdf).

## Controller and scan

Build a CPU-only microSD test image with the Bluetooth probe enabled:

```sh
python tools/build.py --bluetooth 1 --wifi 0 --v3d 0 --usb 0 --ethernet 0 --tls-trust-c none
```

Open Bluetooth from Home and tap Scan. The driver runs a bounded BLE discovery
scan and reports advertising names, addresses and signal strength. It keeps
up to 16 results and replaces them on the next scan. The regular kernel and
the desktop simulator show Bluetooth as unavailable. The simulator exercises
Home navigation without a host-radio dependency.

The driver starts at 115200 baud with hardware flow control. For a board
that needs the Cypress RAM patch, review the license using
`python tools/bluetooth_firmware.py fetch`, then cache and embed the pinned
Pi 4 patch:

```sh
python tools/bluetooth_firmware.py fetch --accept-license
python tools/build.py --bluetooth 1 --bluetooth-patchram out/bluetooth-firmware/BCM4345C0.hcd
```

The kernel checks the controller's ROM identity before applying the patch
and uses the patched controller for legacy BLE advertising discovery.
Test with a known advertising device on the target Pi. Watch UART for
startup failures and record the Pi revision and firmware version.
Pairing, GATT connections, Classic profiles and file transfer are later
protocol work.

## App-facing ABI

`sys_bt_status` returns the controller state and current result count.
`sys_bt_scan_start` queues a scan and `sys_bt_device` reads an indexed result.
The calls are serviced with the same core-0 ownership as the other device
syscalls. The kernel owns the UART, HCI framing and scan state. Future
connection handles can build on the existing discovery data.
