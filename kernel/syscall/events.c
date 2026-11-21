#include <vantage/kernel/events.h>
#include <vantage/kernel/net.h>
#include <vantage/kernel/platform.h>
#include <vantage/drivers/key.h>
#include <vantage/drivers/touch.h>

#ifndef VANTAGE_SIM
#include <vantage/kernel/task.h>
#include <vantage/platform/pi4/drivers.h>
#if VANTAGE_USB
#include <vantage/platform/pi4/usb_input.h>
#endif
#endif

#define INPUT_EVENTS (EVENT_KEY | EVENT_TOUCH)

/* Stage one event from each driver without consuming it on behalf of Home.
 * The driver retains the rest of its queue. All access is serialized on core 0. */
static KeyEvent staged_key;
static TouchContact staged_touch;
static uint32_t input_events;

static uint32_t read_key(KeyEvent* key) {
#if VANTAGE_USB && !defined(VANTAGE_SIM)
    if (pi4_usb_key_read(key)) return 1;
#endif
    return key_read(key);
}

static uint32_t read_touch(TouchContact* contact) {
#if VANTAGE_USB && !defined(VANTAGE_SIM)
    if (pi4_usb_touch_read(contact)) return 1;
#endif
    return touch_read(contact);
}

uint32_t event_read_key(KeyEvent* key) {
    if (!(input_events & EVENT_KEY)) return read_key(key);
    *key = staged_key;
    input_events &= ~EVENT_KEY;
    return 1;
}

uint32_t event_read_touch(TouchContact* contact) {
    if (!(input_events & EVENT_TOUCH)) return read_touch(contact);
    *contact = staged_touch;
    input_events &= ~EVENT_TOUCH;
    return 1;
}

static uint32_t input_ready(uint32_t interests) {
    uint32_t missing = interests & ~input_events;
    if ((missing & EVENT_KEY) && read_key(&staged_key)) input_events |= EVENT_KEY;
    if ((missing & EVENT_TOUCH) && read_touch(&staged_touch)) input_events |= EVENT_TOUCH;
    return input_events & interests;
}

static uint32_t status_changes(void) {
    static NetStatus previous_net;
    static int previous_scan, initialized;
    static uint32_t previous_sync;
    NetStatus net;
    NetTime time;
    net_status_cached(&net);
    int scan = net_scan_state();
    (void)net_time(&time);
    /* Counters, signal strength and the lease countdown do not require a
     * status redraw. Preserve notifications for link and address changes. */
    net.rssi = 0;
    net.lease_seconds = 0;
    net.rx_frames = net.tx_frames = net.rx_dropped = 0;
    uint32_t changes = 0;
    if (initialized && (memcmp(&net, &previous_net, sizeof(net)) ||
                        scan != previous_scan || time.synced != previous_sync)) changes |= EVENT_NETWORK;
    memcpy(&previous_net, &net, sizeof(net));
    previous_scan = scan;
    previous_sync = time.synced;
#if VANTAGE_BLUETOOTH && !defined(VANTAGE_SIM)
    static BtStatus previous_bt;
    BtStatus bt = {0};
    pi4_bt_status(&bt);
    if (initialized && memcmp(&previous_bt, &bt, sizeof(bt))) changes |= EVENT_BLUETOOTH;
    previous_bt = bt;
#endif
    initialized = 1;
    return changes;
}

#ifdef VANTAGE_SIM
static uint32_t simulated_changes;

void event_service(void) {
    net_poll();
    simulated_changes |= status_changes();
}
#else
void event_service(void) {
    pi4_drivers_poll();
    task_notify_events(status_changes() | input_ready(INPUT_EVENTS));
}
#endif

int event_wait(uint32_t interests, uint32_t timeout_us) {
    if ((interests & ~(uint32_t)EVENT_ALL) || timeout_us > EVENT_WAIT_MAX_US) return -1;
#ifdef VANTAGE_SIM
    uint32_t start = time_us();
    for (;;) {
        event_service();
        uint32_t events = (simulated_changes & interests) | input_ready(interests);
        simulated_changes &= ~events;
        uint32_t elapsed = time_us() - start;
        if (events || elapsed >= timeout_us) return (int)events;
        uint32_t delay = timeout_us - elapsed;
        sim_event_delay(delay < EVENT_SERVICE_US ? delay : EVENT_SERVICE_US);
    }
#else
    if (!task_current()) return -1;
    /* Discard old input notifications and take a fresh level reading. Core 0
     * is masked from this check until the scheduler publishes the wait. */
    (void)task_wait_events(interests & INPUT_EVENTS, 0);
    uint32_t ready = input_ready(interests);
    return (int)(ready | task_wait_events(interests, ready ? 0 : timeout_us));
#endif
}
