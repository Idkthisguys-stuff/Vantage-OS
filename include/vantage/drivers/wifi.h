#pragma once
#include <vantage/drivers/sdio.h>

#define WIFI_FRAME_MAX 1514U
#define WIFI_PACKET_MAX 8192U
#define WIFI_NVRAM_MAX 16384U
#define WIFI_SCAN_LIMIT 16U

enum WifiSecurity {
    WIFI_OPEN, WIFI_WEP, WIFI_WPA, WIFI_WPA2, WIFI_WPA3, WIFI_WPA2_WPA3, WIFI_ENTERPRISE, WIFI_OTHER
};

typedef struct {
    uint8_t bssid[6];
    uint8_t ssid_bytes, channel;
    int16_t rssi;
    uint8_t security, reserved;
    unsigned char ssid[32];
} WifiBss;

typedef struct {
    void* context;
    int (*direct)(void*, unsigned int, uint32_t, int, uint8_t*);
    int (*transfer)(void*, unsigned int, uint32_t, int, int, void*, unsigned int);
    uint32_t (*time_us)(void*);
    int (*delay)(void*, uint32_t);
    int (*owned)(void*);
} WifiBus;

typedef struct {
    const unsigned char* firmware;
    size_t firmware_bytes;
    const unsigned char* nvram;
    size_t nvram_bytes;
    const unsigned char* clm;
    size_t clm_bytes;
    char country[3];
    int32_t country_revision;
} WifiFirmware;

typedef struct {
    WifiBus bus;
    int state, busy, error, link, keyed, secure, join_error, associated, connected;
    uint32_t chip, revision, arm, arm_wrap, d11_wrap, sdregs, sd_revision, ram_bytes;
    uint32_t window, control_id, control_command, control_length, control_status;
    uint8_t tx_sequence, rx_sequence, tx_window, flow_mask, mac[6];
    int control_ready, control_set, host_flow, protocol_ready;
    int joining, join_result, open, join_missing;
    uint32_t join_start, scan_start;
    unsigned int join_ssid_bytes;
    unsigned char join_ssid[32];
    int scanning, scanned;
    unsigned int bss_count;
    WifiBss bss[WIFI_SCAN_LIMIT];
    unsigned int rx_head, rx_count, dropped;
    unsigned int frame_bytes[4];
    unsigned char frames[4][WIFI_FRAME_MAX];
    unsigned char tx[WIFI_PACKET_MAX], rx[WIFI_PACKET_MAX], control[4096];
    unsigned char nvram[WIFI_NVRAM_MAX];
    unsigned char erom[4096];
} Wifi;

WifiBus wifi_sdio_bus(Sdio* host);
int wifi_nvram_pack(const unsigned char* text, size_t bytes, unsigned char* output,
                     size_t capacity, size_t* packed_bytes);
/* CYW43455 only (chip 0x4345 revision 6), polling, firmware WPA2-PSK offload.
 * Buffers and state must be private kernel memory, retained for driver lifetime.
 * Firmware/NVRAM/CLM must match this board; country is explicit, not guessed. */
int wifi_boot(Wifi* wifi, const WifiBus* bus, const WifiFirmware* firmware);
/* Services one SDPCM packet and advances a pending join or scan. Returns
 * 1 after a packet, 0 when idle, or a negative HwResult that stops the radio. */
int wifi_poll(Wifi* wifi);
/* Configures security and requests association without waiting for it.
 * passphrase_bytes 0 explicitly selects an open network; otherwise the
 * WPA2-PSK passphrase is 8..63 printable ASCII bytes. WIFI_JOIN_HIDDEN
 * probes for the SSID instead of relying on its beacons. */
#define WIFI_JOIN_HIDDEN 1U
int wifi_join_start(Wifi* wifi, const unsigned char* ssid, unsigned int ssid_bytes,
                     const unsigned char* passphrase, unsigned int passphrase_bytes, unsigned int flags);
/* HW_BUSY while joining, HW_OK while connected, HW_OFF when not associated,
 * or the HwResult that ended the last join; join_missing is then set when
 * the firmware found no network with that SSID. */
int wifi_join_status(const Wifi* wifi);
/* Blocking join: wifi_join_start followed by polling to completion. */
int wifi_join(Wifi* wifi, const unsigned char* ssid, unsigned int ssid_bytes,
               const unsigned char* passphrase, unsigned int passphrase_bytes);
int wifi_disconnect(Wifi* wifi);
/* Active scan of all channels. Results replace the previous scan. */
int wifi_scan_start(Wifi* wifi);
/* HW_BUSY while scanning, HW_OK when results are complete, HW_OFF if none. */
int wifi_scan_status(const Wifi* wifi);
int wifi_rssi(Wifi* wifi, int32_t* dbm);
int wifi_send(Wifi* wifi, const void* ethernet, unsigned int bytes);
/* Returns byte count, zero when no packet is queued, or a negative HwResult. */
int wifi_receive(Wifi* wifi, void* ethernet, unsigned int capacity);
