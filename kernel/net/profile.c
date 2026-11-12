/* Saved Wi-Fi networks for automatic connection, in kernel memory only.
 * Passphrases never leave the kernel and are erased with their profile. */
#include "internal.h"

static NetSavedProfile profiles[NET_PROFILE_LIMIT];
static unsigned int count;

static void erase(void* memory, size_t bytes) {
    volatile unsigned char* p = memory;
    while (bytes--) *p++ = 0;
}

unsigned int net_profile_count(void) { return count; }

const NetSavedProfile* net_profile_at(unsigned int index) { return index < count ? &profiles[index] : NULL; }

void net_profile_reset(void) {
    erase(profiles, sizeof(profiles));
    count = 0;
}

static int find(const char* ssid, uint32_t bytes) {
    for (unsigned int i = 0; i < count; i++)
        if (profiles[i].ssid_bytes == bytes && !memcmp(profiles[i].ssid, ssid, bytes)) return (int)i;
    return -1;
}

static int add_profile(const NetProfile* request) {
    if ((request->flags & ~NET_PROFILE_HIDDEN) ||
        !net_credentials_valid(request->ssid_length, request->passphrase, request->passphrase_length)) return NET_BAD;
    int index = find(request->ssid, request->ssid_length);
    if (index < 0) {
        if (count == NET_PROFILE_LIMIT) return NET_NO_SPACE;
        index = (int)count++;
    }
    NetSavedProfile* p = &profiles[index];
    erase(p, sizeof(*p));
    p->flags = request->flags;
    p->ssid_bytes = request->ssid_length;
    p->passphrase_bytes = request->passphrase_length;
    memcpy(p->ssid, request->ssid, request->ssid_length);
    memcpy(p->passphrase, request->passphrase, request->passphrase_length);
    net_autojoin_changed(1);
    return 0;
}

static int delete_profile(const NetProfile* request) {
    if (!request->ssid_length || request->ssid_length > NET_SSID_MAX) return NET_BAD;
    int index = find(request->ssid, request->ssid_length);
    if (index < 0) return NET_NOT_FOUND;
    for (unsigned int i = (unsigned int)index; i + 1 < count; i++) profiles[i] = profiles[i + 1];
    erase(&profiles[--count], sizeof(profiles[0]));
    net_autojoin_changed(0);
    return 0;
}

static int list_profile(NetProfile* request) {
    uint32_t cursor = request->cursor;
    if (cursor >= count) return 0;
    const NetSavedProfile* p = &profiles[cursor];
    memset(request, 0, sizeof(*request));
    request->operation = NET_PROFILE_LIST;
    request->cursor = cursor + 1U;
    request->flags = p->flags;
    request->ssid_length = p->ssid_bytes;
    request->passphrase_length = p->passphrase_bytes ? 1U : 0U;
    memcpy(request->ssid, p->ssid, p->ssid_bytes);
    return 1;
}

int net_profile(NetProfile* request) {
    if (!net_wifi_usable()) return NET_UNAVAILABLE;
    switch (request->operation) {
    case NET_PROFILE_ADD: return add_profile(request);
    case NET_PROFILE_REMOVE: return delete_profile(request);
    case NET_PROFILE_LIST: return list_profile(request);
    case NET_PROFILE_CLEAR:
        net_profile_reset();
        net_autojoin_changed(0);
        return 0;
    default:
        return NET_BAD;
    }
}
