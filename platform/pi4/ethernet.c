/* Pi 4 network glue: the GENET wired MAC as the network service's Ethernet link. */
#include <vantage/drivers/genet.h>
#include <vantage/platform/pi4/drivers.h>

static Genet genet;

static int link_poll(void* context) {
    int result = genet_poll(context);
    return result < 0 ? NET_IO : result;
}

static int link_send(void* context, const void* frame, unsigned int bytes) {
    switch (genet_send(context, frame, bytes)) {
    case HW_OK: return 0;
    case HW_OFF: return NET_OFFLINE;
    case HW_INVALID: return NET_BAD;
    case HW_BUSY: return NET_BUSY;
    default: return NET_IO;
    }
}

static int link_receive(void* context, void* frame, unsigned int capacity) {
    int result = genet_receive(context, frame, capacity);
    return result >= 0 ? result : result == HW_NO_SPACE ? NET_NO_SPACE : result == HW_OFF ? NET_OFFLINE : NET_IO;
}

static void link_address(void* context, unsigned char mac[6]) { memcpy(mac, ((const Genet*)context)->mac, 6); }

static int link_carrier(void* context, uint32_t* mbps) { return genet_carrier(context, mbps); }

int pi4_ethernet_start(const HwOps* io, const NetLink** link) {
    static NetLink ethernet = {
        .kind = NET_LINK_KIND_ETHERNET, .poll = link_poll, .send = link_send, .receive = link_receive,
        .address = link_address, .carrier = link_carrier,
    };
    *link = NULL;
    int result = genet_init(&genet, io);
    if (result != HW_OK) return result;
    ethernet.context = &genet;
    *link = &ethernet;
    return HW_OK;
}
