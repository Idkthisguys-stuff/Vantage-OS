#pragma once
#include <vantage/abi/bluetooth.h>
#include <vantage/drivers/wifi.h>
#include <vantage/kernel/net.h>

const HwOps* pi4_hardware_ops(void);

void pi4_drivers_start(void);
/* Services the network link and protocol timers (core 0, masked). */
void pi4_drivers_poll(void);
/* NULL unless explicitly enabled and successfully booted. Kernel-only. */
Wifi* pi4_wifi_device(void);
/* The booted radio as the network service's link. */
const NetLink* pi4_net_link(Wifi* wifi);
/* Brings up GENET Ethernet without waiting for a cable: HW_OK with *link set,
 * or the HwResult (HW_OFF when the controller is absent) with *link NULL. */
int pi4_ethernet_start(const HwOps* io, const NetLink** link);
/* Enables the RNG200 and checks that it produces data. */
int pi4_rng_start(const HwOps* io);

/* Core 0 owns UART0 and the Bluetooth HCI state. */
int pi4_bt_start(const HwOps* io, const char** stage);
/* Reports transitions to HCI-ready (BT_OK) or failed (negative). */
int pi4_bt_poll(void);
void pi4_bt_status(BtStatus* status);
int pi4_bt_scan_start(void);
int pi4_bt_device(uint32_t index, BtDevice* device);
