/* Pi 4 network glue: the CYW43455 station as a NetLink, and the BCM2711
 * RNG200 entropy source (registers per Linux v6.12 iproc-rng200). */
#include <vantage/kernel/net.h>
#include <vantage/platform/pi4/drivers.h>

#define RNG_BASE 0xfe104000ULL
#define RNG_CTRL 0x00U
#define RNG_SOFT_RESET 0x04U
#define RBG_SOFT_RESET 0x08U
#define RNG_INT_STATUS 0x18U
#define RNG_FIFO_DATA 0x20U
#define RNG_FIFO_COUNT 0x24U
#define RNG_FAILED 0x80000020U

static const HwOps* rng_io;
static uint64_t weak_state;

static uint32_t rng_read(unsigned int offset) { return rng_io->read(rng_io->context, RNG_BASE + offset); }
static void rng_write(unsigned int offset, uint32_t value) { rng_io->write(rng_io->context, RNG_BASE + offset, value); }

static void rng_enable(int enable) {
    rng_write(RNG_CTRL, (rng_read(RNG_CTRL) & ~0x1fffU) | (enable ? 1U : 0U));
}

static void rng_restart(void) {
    rng_enable(0);
    rng_write(RNG_INT_STATUS, 0xffffffffU);
    rng_write(RBG_SOFT_RESET, rng_read(RBG_SOFT_RESET) | 1U);
    rng_write(RNG_SOFT_RESET, rng_read(RNG_SOFT_RESET) | 1U);
    rng_write(RNG_SOFT_RESET, rng_read(RNG_SOFT_RESET) & ~1U);
    rng_write(RBG_SOFT_RESET, rng_read(RBG_SOFT_RESET) & ~1U);
    rng_enable(1);
}

/* One word, or 0 with *ok cleared if the FIFO stays empty for 100 ms. */
static uint32_t rng_word(int* ok) {
    uint32_t start = rng_io->time_us(rng_io->context);
    int restarted = 0;
    for (unsigned int i = 0; i < 1000000U; i++) {
        /* A health-test failure is reset once; data is only trusted after
         * the status reads clean again. */
        if (rng_read(RNG_INT_STATUS) & RNG_FAILED) {
            if (restarted) break;
            rng_restart();
            restarted = 1;
            continue;
        }
        if (rng_read(RNG_FIFO_COUNT) & 0xffU) { *ok = 1; return rng_read(RNG_FIFO_DATA); }
        if (rng_io->time_us(rng_io->context) - start >= 100000U) break;
    }
    *ok = 0;
    return 0;
}

int pi4_rng_start(const HwOps* io) {
    if (!hw_ops_valid(io)) return HW_INVALID;
    rng_io = io;
    rng_enable(1);
    int ok = 0;
    uint32_t first = rng_word(&ok), second = ok ? rng_word(&ok) : 0;
    weak_state ^= ((uint64_t)io->time_us(io->context) << 32) | 0x9e3779b9U;
    if (!ok || first == second) { rng_io = NULL; return HW_IO; }
    return HW_OK;
}

uint32_t net_port_random(void) {
    if (rng_io) {
        int ok = 0;
        uint32_t value = rng_word(&ok);
        if (ok) return value;
        rng_io = NULL;
    }
    /* Without the hardware source: xorshift64* stirred with the free-running
     * timer. Adequate for collision avoidance only, not unpredictability. */
    const HwOps* io = pi4_hardware_ops();
    weak_state ^= io->time_us(io->context) + 0x9e3779b97f4a7c15ULL;
    weak_state ^= weak_state >> 12; weak_state ^= weak_state << 25; weak_state ^= weak_state >> 27;
    return (uint32_t)((weak_state * 0x2545f4914f6cdd1dULL) >> 32);
}

int net_entropy(void* data, unsigned int bytes) {
    if (!rng_io) return NET_UNAVAILABLE;
    unsigned char* out = data;
    unsigned char words[NET_RANDOM_MAX];
    if (bytes > sizeof(words)) return NET_BAD;
    for (unsigned int i = 0; i < bytes; i += 4) {
        int ok = 0;
        uint32_t value = rng_word(&ok);
        if (!ok) { rng_io = NULL; memset(words, 0, sizeof(words)); return NET_UNAVAILABLE; }
        for (unsigned int k = 0; k < 4 && i + k < bytes; k++) words[i + k] = (unsigned char)(value >> (8 * k));
    }
    memcpy(out, words, bytes);
    memset(words, 0, sizeof(words));
    return 0;
}

static int link_poll(void* context) {
    int result = wifi_poll(context);
    return result < 0 ? NET_IO : result;
}

static int link_send(void* context, const void* frame, unsigned int bytes) {
    int result = wifi_send(context, frame, bytes);
    return result == HW_OK ? 0 : result == HW_OFF ? NET_OFFLINE : result == HW_INVALID ? NET_BAD : NET_IO;
}

static int link_receive(void* context, void* frame, unsigned int capacity) {
    int result = wifi_receive(context, frame, capacity);
    return result >= 0 ? result : result == HW_NO_SPACE ? NET_NO_SPACE : NET_OFFLINE;
}

static int link_join(void* context, const unsigned char* ssid, unsigned int ssid_bytes,
                     const unsigned char* passphrase, unsigned int passphrase_bytes, uint32_t flags) {
    if (flags & ~NET_JOIN_HIDDEN) return NET_BAD;
    int result = wifi_join_start(context, ssid, ssid_bytes, passphrase, passphrase_bytes,
                                 (flags & NET_JOIN_HIDDEN) ? WIFI_JOIN_HIDDEN : 0);
    if (result == HW_OK) return 0;
    if (result == HW_INVALID) return NET_BAD;
    if (result == HW_BUSY) return NET_BUSY;
    return NET_IO;
}

static int link_join_status(void* context) {
    switch (wifi_join_status(context)) {
    case HW_OK: return 1;
    case HW_BUSY: return NET_AGAIN;
    case HW_OFF: return NET_OFFLINE;
    case HW_IO: return ((const Wifi*)context)->join_missing ? NET_NOT_FOUND : NET_AUTH;
    case HW_TIMEOUT: return NET_TIMEOUT;
    default: return NET_IO;
    }
}

static int link_leave(void* context) { return wifi_disconnect(context) == HW_OK ? 0 : NET_IO; }

static int link_scan(void* context) {
    int result = wifi_scan_start(context);
    return result == HW_OK ? 0 : result == HW_BUSY ? NET_BUSY : NET_IO;
}

static int link_scan_status(void* context) {
    int result = wifi_scan_status(context);
    return result == HW_BUSY ? NET_AGAIN : result == HW_OK ? 1 : 0;
}

static int link_scan_entry(void* context, unsigned int index, NetScan* entry) {
    static const uint32_t security[] = {
        NET_SECURITY_OPEN, NET_SECURITY_WEP, NET_SECURITY_WPA, NET_SECURITY_WPA2,
        NET_SECURITY_WPA3, NET_SECURITY_WPA2_WPA3, NET_SECURITY_ENTERPRISE, NET_SECURITY_OTHER
    };
    const Wifi* wifi = context;
    if (index >= wifi->bss_count) return 0;
    const WifiBss* bss = &wifi->bss[index];
    memset(entry, 0, sizeof(*entry));
    entry->rssi = bss->rssi;
    entry->channel = bss->channel;
    entry->security = bss->security <= WIFI_OTHER ? security[bss->security] : NET_SECURITY_OTHER;
    entry->ssid_length = bss->ssid_bytes;
    memcpy(entry->ssid, bss->ssid, bss->ssid_bytes);
    return 1;
}

static int link_rssi(void* context, int32_t* dbm) { return wifi_rssi(context, dbm) == HW_OK ? 0 : NET_IO; }

static void link_address(void* context, unsigned char mac[6]) { memcpy(mac, ((const Wifi*)context)->mac, 6); }

const NetLink* pi4_net_link(Wifi* wifi) {
    static NetLink link = {
        .kind = NET_LINK_KIND_WIFI, .poll = link_poll, .send = link_send, .receive = link_receive,
        .address = link_address, .join = link_join, .join_status = link_join_status, .leave = link_leave,
        .scan = link_scan, .scan_status = link_scan_status, .scan_entry = link_scan_entry, .rssi = link_rssi,
    };
    link.context = wifi;
    return &link;
}
