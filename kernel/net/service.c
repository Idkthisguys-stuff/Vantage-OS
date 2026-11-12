/* Network service: owns the attached links, Wi-Fi association, its reconnect
 * and automatic-connection policy, chooses the interface that carries IPv4,
 * and drives the protocol timers from net_poll. */
#include "internal.h"
#include <vantage/kernel/platform.h>

#define POLL_PACKETS 32U
#define RETRY_FIRST_MS 2000U
#define RETRY_MAX_MS 60000U
#define AUTO_FIRST_MS 30000U
#define AUTO_MAX_MS 300000U
#define AUTO_RETRIES 3U
#define CARRIER_MS 500U

enum { AUTO_WAIT, AUTO_SCANNING, AUTO_JOINING };

static const NetLink *wifi, *ethernet, *active;
static struct {
    int state, error, failed, credentials, connected_once, hidden, from_profile;
    int32_t rssi;
    uint32_t retries, attempts, backoff;
    uint64_t retry_at, rssi_at;
    unsigned int ssid_bytes, passphrase_bytes;
    unsigned char ssid[NET_SSID_MAX], passphrase[64];
} service;
/* Automatic connection: candidates are profile indices, strongest visible
 * network first, then hidden profiles. */
static struct {
    int state, paused, rescan;
    uint32_t backoff;
    uint64_t at;
    unsigned int count, next;
    unsigned char order[NET_PROFILE_LIMIT];
} autojoin;
static struct { int failed, up, checked; uint32_t mbps; uint64_t checked_at; } wired;
static struct { int started; uint32_t last; uint64_t remainder, ms; } clock;
static unsigned char frame[ETH_FRAME_MAX];

static void erase(void* memory, size_t bytes) {
    volatile unsigned char* p = memory;
    while (bytes--) *p++ = 0;
}

uint64_t net_now(void) {
    uint32_t now = time_us();
    if (!clock.started) { clock.started = 1; clock.last = now; }
    clock.remainder += (uint32_t)(now - clock.last);
    clock.last = now;
    clock.ms += clock.remainder / 1000U;
    clock.remainder %= 1000U;
    return clock.ms;
}

uint32_t net_random(void) { return net_port_random(); }

uint16_t net_port_ephemeral(void) {
    for (unsigned int i = 0; i < 128; i++) {
        uint16_t port = (uint16_t)(49152U + net_random() % 16384U);
        if (!net_socket_port_in_use(port) && !net_dns_port_in_use(port) && !net_sntp_port_in_use(port)) return port;
    }
    return 0;
}

static int wifi_up(void) { return wifi && !service.failed && service.state == NET_LINK_UP; }
static int wired_up(void) { return ethernet && !wired.failed && wired.up; }

int net_link_ready(void) {
    if (!active) return 0;
    return active == ethernet ? wired_up() : wifi_up();
}

int net_link_send(const void* data, unsigned int bytes) {
    if (!net_link_ready()) return NET_OFFLINE;
    int result = active->send(active->context, data, bytes);
    return result < 0 ? result : 0;
}

void net_address_bound(uint32_t previous) {
    if (previous && previous != net_interface.address) {
        net_ip_reset(); net_dns_reset(); net_socket_reset();
    }
}

void net_address_lost(void) {
    net_interface.bound = 0;
    net_interface.address = net_interface.netmask = net_interface.gateway = 0;
    net_interface.dns[0] = net_interface.dns[1] = 0;
    net_interface.ntp[0] = net_interface.ntp[1] = 0;
    net_ip_reset(); net_dns_reset(); net_socket_reset(); net_sntp_reset();
}

int net_credentials_valid(uint32_t ssid_bytes, const char* passphrase, uint32_t passphrase_bytes) {
    if (!ssid_bytes || ssid_bytes > NET_SSID_MAX ||
        (passphrase_bytes && (passphrase_bytes < NET_PASSPHRASE_MIN || passphrase_bytes > NET_PASSPHRASE_MAX)))
        return 0;
    for (unsigned int i = 0; i < passphrase_bytes; i++)
        if (passphrase[i] < 32 || passphrase[i] > 126) return 0;
    return 1;
}

int net_wifi_usable(void) { return wifi && !service.failed; }

/* Stops IPv4 on the active link; the next poll selects another one. */
static void deactivate(int release) {
    if (!active) return;
    net_dhcp_stop(release);
    net_address_lost();
    active = NULL;
}

static void select_interface(void) {
    const NetLink* next = wired_up() ? ethernet : wifi_up() ? wifi : NULL;
    if (next == active) return;
    deactivate(0);
    if (!next) return;
    active = next;
    active->address(active->context, net_interface.mac);
    net_dhcp_start();
}

static void forget(void) {
    erase(service.ssid, sizeof(service.ssid));
    erase(service.passphrase, sizeof(service.passphrase));
    service.ssid_bytes = service.passphrase_bytes = 0;
    service.credentials = service.connected_once = service.hidden = service.from_profile = 0;
}

static void wifi_down(int release) {
    service.rssi = 0;
    if (active && active == wifi) deactivate(release);
}

static void retry_later(int error) {
    service.state = NET_LINK_RETRYING;
    service.error = error;
    service.retry_at = net_now() + service.backoff;
    service.backoff = service.backoff * 2U > RETRY_MAX_MS ? RETRY_MAX_MS : service.backoff * 2U;
}

static int begin_join(void) {
    int result = wifi->join(wifi->context, service.ssid, service.ssid_bytes,
                            service.passphrase_bytes ? service.passphrase : NULL, service.passphrase_bytes,
                            service.hidden ? NET_JOIN_HIDDEN : 0);
    if (result >= 0) service.state = NET_LINK_JOINING;
    return result;
}

static void autojoin_wait(uint64_t delay) {
    autojoin.state = AUTO_WAIT;
    autojoin.count = autojoin.next = 0;
    autojoin.at = net_now() + delay;
}

static void autojoin_exhausted(void) {
    if (autojoin.rescan) { autojoin.rescan = 0; autojoin_wait(0); return; }
    autojoin_wait(autojoin.backoff);
    autojoin.backoff = autojoin.backoff * 2U > AUTO_MAX_MS ? AUTO_MAX_MS : autojoin.backoff * 2U;
}

static void autojoin_next(void) {
    while (autojoin.next < autojoin.count) {
        const NetSavedProfile* p = net_profile_at(autojoin.order[autojoin.next++]);
        if (!p) continue;
        forget();
        memcpy(service.ssid, p->ssid, p->ssid_bytes);
        memcpy(service.passphrase, p->passphrase, p->passphrase_bytes);
        service.ssid_bytes = p->ssid_bytes;
        service.passphrase_bytes = p->passphrase_bytes;
        service.credentials = service.from_profile = 1;
        service.hidden = (p->flags & NET_PROFILE_HIDDEN) != 0;
        service.retries = service.attempts = 0;
        service.backoff = RETRY_FIRST_MS;
        int result = begin_join();
        if (result >= 0) { autojoin.state = AUTO_JOINING; service.error = 0; return; }
        service.error = result;
        forget();
    }
    autojoin_exhausted();
}

static int compatible(const NetSavedProfile* p, uint32_t security) {
    if (!p->passphrase_bytes) return security == NET_SECURITY_OPEN;
    return security == NET_SECURITY_WPA || security == NET_SECURITY_WPA2 || security == NET_SECURITY_WPA2_WPA3;
}

static void autojoin_choose(void) {
    int32_t strength[NET_PROFILE_LIMIT];
    int seen[NET_PROFILE_LIMIT] = {0};
    unsigned int profiles = net_profile_count();
    for (unsigned int i = 0; i < NET_SCAN_LIMIT; i++) {
        NetScan entry;
        memset(&entry, 0, sizeof(entry));
        if (wifi->scan_entry(wifi->context, i, &entry) != 1) break;
        if (!entry.ssid_length || entry.ssid_length > NET_SSID_MAX) continue;
        for (unsigned int k = 0; k < profiles; k++) {
            const NetSavedProfile* p = net_profile_at(k);
            if ((p->flags & NET_PROFILE_HIDDEN) || p->ssid_bytes != entry.ssid_length ||
                memcmp(p->ssid, entry.ssid, p->ssid_bytes) || !compatible(p, entry.security)) continue;
            if (!seen[k] || entry.rssi > strength[k]) { seen[k] = 1; strength[k] = entry.rssi; }
        }
    }
    autojoin.count = autojoin.next = 0;
    for (unsigned int k = 0; k < profiles; k++) {
        if (!seen[k]) continue;
        unsigned int j = autojoin.count++;
        for (; j && strength[autojoin.order[j - 1]] < strength[k]; j--) autojoin.order[j] = autojoin.order[j - 1];
        autojoin.order[j] = (unsigned char)k;
    }
    for (unsigned int k = 0; k < profiles; k++)
        if (net_profile_at(k)->flags & NET_PROFILE_HIDDEN) autojoin.order[autojoin.count++] = (unsigned char)k;
    autojoin_next();
}

/* Runs while Wi-Fi is down, not paused and profiles exist: scan (or adopt a
 * scan already running), then try the candidates in order. */
static void autojoin_poll(void) {
    if (autojoin.state == AUTO_JOINING) return;
    if (!net_wifi_usable() || autojoin.paused || service.state != NET_LINK_DOWN || !net_profile_count()) {
        if (autojoin.state == AUTO_SCANNING) autojoin_wait(0);
        return;
    }
    if (autojoin.state == AUTO_WAIT) {
        if (net_now() < autojoin.at) return;
        if (wifi->scan_status(wifi->context) != NET_AGAIN && wifi->scan(wifi->context) < 0) {
            autojoin_exhausted();
            return;
        }
        autojoin.state = AUTO_SCANNING;
        autojoin.rescan = 0;
        return;
    }
    int status = wifi->scan_status(wifi->context);
    if (status == NET_AGAIN) return;
    if (status != 1) { autojoin_exhausted(); return; }
    autojoin_choose();
}

void net_autojoin_changed(int added) {
    if (autojoin.state == AUTO_JOINING) { autojoin.count = autojoin.next; autojoin.rescan = 1; }
    if (!added) return;
    autojoin.paused = 0;
    autojoin.backoff = AUTO_FIRST_MS;
    if (autojoin.state == AUTO_WAIT) autojoin.at = net_now();
}

static void wifi_progress(void) {
    int status = wifi->join_status(wifi->context);
    if (service.state == NET_LINK_JOINING && status == 1) {
        service.state = NET_LINK_UP;
        service.connected_once = 1;
        service.error = 0;
        service.attempts = 0;
        service.backoff = RETRY_FIRST_MS;
        if (service.from_profile) { autojoin_wait(0); autojoin.backoff = AUTO_FIRST_MS; }
    } else if (service.state == NET_LINK_JOINING && status != NET_AGAIN) {
        int error = status < 0 ? status : NET_IO;
        if (service.connected_once) { retry_later(error); return; }
        int automatic = service.from_profile;
        service.state = NET_LINK_DOWN;
        service.error = error;
        forget();
        if (automatic) autojoin_next();
        else autojoin_wait(autojoin.backoff);
    } else if (service.state == NET_LINK_UP && status != 1) {
        wifi_down(0);
        service.retries++;
        retry_later(NET_OFFLINE);
    } else if (service.state == NET_LINK_RETRYING && net_now() >= service.retry_at) {
        if (service.from_profile && service.attempts >= AUTO_RETRIES) {
            /* A saved network that stays away: choose again from a new scan. */
            service.state = NET_LINK_DOWN;
            forget();
            autojoin_wait(0);
            return;
        }
        service.retries++;
        service.attempts++;
        int result = begin_join();
        if (result < 0) retry_later(result);
    }
}

void net_attach(const NetLink* wifi_link, const NetLink* ethernet_link) {
    if (active) deactivate(0);
    wifi = wifi_link;
    ethernet = ethernet_link;
    active = NULL;
    erase(&service, sizeof(service));
    memset(&autojoin, 0, sizeof(autojoin));
    memset(&wired, 0, sizeof(wired));
    autojoin.backoff = AUTO_FIRST_MS;
    net_profile_reset();
    net_sntp_reset();
    memset(&net_interface, 0, sizeof(net_interface));
    if (wifi) wifi->address(wifi->context, net_interface.mac);
    else if (ethernet) ethernet->address(ethernet->context, net_interface.mac);
    (void)net_now();
}

static void fail_device(void) {
    wifi_down(0);
    forget();
    service.failed = 1;
    service.state = NET_LINK_DOWN;
    service.error = NET_IO;
}

static void ethernet_lost(void) {
    wired.failed = 1;
    wired.up = 0;
    wired.mbps = 0;
    if (active && active == ethernet) deactivate(0);
}

/* Services a device and delivers its frames while it is the active link;
 * frames from the other link are discarded. */
static int service_link(const NetLink* device) {
    for (unsigned int i = 0; i < POLL_PACKETS; i++) {
        int progress = device->poll(device->context);
        if (progress < 0) return progress;
        int received;
        for (unsigned int j = 0; j < 8 && (received = device->receive(device->context, frame, sizeof(frame))) > 0; j++)
            if (device == active) net_frame_input(frame, (unsigned int)received);
        if (!progress) break;
    }
    return 0;
}

void net_poll(void) {
    uint64_t now = net_now();
    if (ethernet && !wired.failed) {
        if (service_link(ethernet) < 0) ethernet_lost();
        else if (!wired.checked || now - wired.checked_at >= CARRIER_MS) {
            /* The carrier query may cost an MDIO transaction: rate-limited. */
            uint32_t mbps = 0;
            wired.up = ethernet->carrier && ethernet->carrier(ethernet->context, &mbps) == 1;
            wired.mbps = wired.up ? mbps : 0;
            wired.checked = 1;
            wired.checked_at = now;
        }
    }
    if (wifi && !service.failed && service_link(wifi) < 0) fail_device();
    if (wifi && !service.failed) wifi_progress();
    autojoin_poll();
    select_interface();
    if (net_link_ready()) {
        net_dhcp_poll();
        net_ip_poll();
        net_sntp_poll();
    }
    net_dns_poll();
    net_socket_poll();
}

int net_status(NetStatus* status) {
    memset(status, 0, sizeof(*status));
    if (wifi) {
        status->available = !service.failed;
        status->link = (uint32_t)service.state;
        status->error = service.error;
        status->retries = service.retries;
        if (service.credentials) {
            status->ssid_length = service.ssid_bytes;
            memcpy(status->ssid, service.ssid, service.ssid_bytes);
            status->security = service.passphrase_bytes ? NET_SECURITY_WPA2 : NET_SECURITY_OPEN;
            status->flags = (service.hidden ? NET_STATUS_HIDDEN : 0U) | (service.from_profile ? NET_STATUS_PROFILE : 0U);
        }
        if (wifi_up()) {
            /* Each reading is a firmware round trip; refresh at most once a second. */
            uint64_t now = net_now();
            if (!service.rssi || now - service.rssi_at >= 1000U) {
                int32_t rssi = 0;
                service.rssi = wifi->rssi(wifi->context, &rssi) >= 0 ? rssi : 0;
                service.rssi_at = now;
            }
            status->rssi = service.rssi;
        }
    }
    if (ethernet) {
        status->ethernet = wired.failed ? NET_ETHERNET_ABSENT : wired.up ? NET_ETHERNET_UP : NET_ETHERNET_NO_CABLE;
        if (wired_up()) status->ethernet_mbps = wired.mbps;
    }
    if (active) status->interface = active == ethernet ? NET_INTERFACE_ETHERNET : NET_INTERFACE_WIFI;
    status->configuration = net_dhcp_state();
    if (net_interface.bound) {
        status->address = net_interface.address;
        status->netmask = net_interface.netmask;
        status->gateway = net_interface.gateway;
        status->dns = net_interface.dns[0];
        status->lease_seconds = net_dhcp_lease_seconds();
    }
    status->rx_frames = net_interface.rx_frames;
    status->tx_frames = net_interface.tx_frames;
    status->rx_dropped = net_interface.rx_dropped;
    return 0;
}

int net_scan(NetScan* request) {
    if (!net_wifi_usable()) return NET_UNAVAILABLE;
    if (request->operation == NET_SCAN_START) {
        /* An automatic scan is already collecting results: share it. */
        if (autojoin.state == AUTO_SCANNING) return 0;
        int result = wifi->scan(wifi->context);
        return result < 0 ? result : 0;
    }
    if (request->operation != NET_SCAN_RESULT) return NET_BAD;
    int status = wifi->scan_status(wifi->context);
    if (status == NET_AGAIN) return NET_AGAIN;
    if (status < 0) return status;
    if (!status) return 0;
    /* Report the strongest networks first. */
    NetScan entries[NET_SCAN_LIMIT];
    unsigned int count = 0;
    memset(entries, 0, sizeof(entries));
    while (count < NET_SCAN_LIMIT && wifi->scan_entry(wifi->context, count, &entries[count]) == 1) count++;
    for (unsigned int i = 1; i < count; i++) {
        NetScan entry = entries[i];
        unsigned int j = i;
        for (; j && entries[j - 1].rssi < entry.rssi; j--) entries[j] = entries[j - 1];
        entries[j] = entry;
    }
    if (request->cursor >= count) return 0;
    uint32_t cursor = request->cursor;
    *request = entries[cursor];
    request->operation = NET_SCAN_RESULT;
    request->cursor = cursor + 1U;
    return 1;
}

int net_join(const NetJoin* request) {
    if (!net_wifi_usable()) return NET_UNAVAILABLE;
    if (request->flags & ~NET_JOIN_HIDDEN) return NET_BAD;
    if (!net_credentials_valid(request->ssid_length, request->passphrase, request->passphrase_length)) return NET_BAD;
    if (service.state != NET_LINK_DOWN) {
        wifi_down(service.state == NET_LINK_UP);
        (void)wifi->leave(wifi->context);
    }
    forget();
    autojoin.paused = 0;
    autojoin.backoff = AUTO_FIRST_MS;
    autojoin_wait(AUTO_FIRST_MS);
    memcpy(service.ssid, request->ssid, request->ssid_length);
    memcpy(service.passphrase, request->passphrase, request->passphrase_length);
    service.ssid_bytes = request->ssid_length;
    service.passphrase_bytes = request->passphrase_length;
    service.hidden = (request->flags & NET_JOIN_HIDDEN) != 0;
    service.credentials = 1;
    service.error = 0;
    service.retries = service.attempts = 0;
    service.backoff = RETRY_FIRST_MS;
    service.state = NET_LINK_DOWN;
    int result = begin_join();
    if (result < 0) { service.error = result; forget(); }
    return result < 0 ? result : 0;
}

int net_leave(void) {
    if (!net_wifi_usable()) return NET_UNAVAILABLE;
    if (service.state != NET_LINK_DOWN) {
        wifi_down(service.state == NET_LINK_UP);
        (void)wifi->leave(wifi->context);
    }
    forget();
    autojoin.paused = 1;
    autojoin_wait(0);
    service.state = NET_LINK_DOWN;
    service.error = 0;
    return 0;
}

int net_parse_ipv4(const char* text, uint32_t* address) {
    uint32_t value = 0;
    for (unsigned int part = 0; part < 4; part++) {
        unsigned int octet = 0, digits = 0;
        while (*text >= '0' && *text <= '9') {
            octet = octet * 10U + (unsigned int)(*text++ - '0');
            if (++digits > 3 || octet > 255) return 0;
        }
        if (!digits || (part < 3 && *text++ != '.')) return 0;
        value = (value << 8) | octet;
    }
    if (*text) return 0;
    *address = value;
    return 1;
}

int net_resolve(NetResolve* request) {
    if (request->flags) return NET_BAD;
    if (net_parse_ipv4(request->name, &request->address)) return 1;
    if (!net_wifi_usable() && (!ethernet || wired.failed)) return NET_UNAVAILABLE;
    if (!net_interface.bound) return NET_OFFLINE;
    return net_dns_resolve(request->name, &request->address);
}
