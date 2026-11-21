#pragma once
#include <vantage/base.h>

#define SYS_BT_STATUS 64U
#define SYS_BT_SCAN_START 65U
#define SYS_BT_DEVICE 66U

#define BT_DEVICE_LIMIT 16U
#define BT_NAME_BYTES 32U

enum { BT_DISABLED, BT_BOOTING, BT_READY, BT_SCANNING, BT_FAILED };
enum { BT_OK = 1, BT_UNAVAILABLE = -1, BT_BUSY = -2, BT_IO = -3,
       BT_TIMEOUT = -4, BT_PROTOCOL = -5 };

typedef struct {
    uint32_t state, count;
    int32_t error;
    uint32_t reserved;
} BtStatus;

/* Address bytes are in HCI order (least significant first).
 * A name may be absent. RSSI 127 denotes an unavailable measurement. */
typedef struct {
    uint8_t address[6], address_type, reserved;
    int32_t rssi;
    char name[BT_NAME_BYTES];
} BtDevice;

typedef struct {
    uint32_t index, reserved;
    BtDevice device;
} BtDeviceCall;
