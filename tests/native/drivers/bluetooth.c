#include <vantage/drivers/bluetooth.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

typedef struct {
    uint32_t now;
    int send_result, receive_error;
    unsigned int sends, attempts;
    size_t tx_size, rx_used, rx_size;
    uint8_t tx[32], rx[1024];
} MockH4;

typedef struct {
    BtHci hci;
    MockH4 mock;
} Fixture;

static const uint8_t reset[] = {1, 0x03, 0x0c, 0};
static const uint8_t event_mask[] = {1, 0x01, 0x0c, 8, 0, 0x80, 0, 0, 0, 0, 0, 0x20};
static const uint8_t le_event_mask[] = {1, 0x01, 0x20, 8, 2, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t scan_parameters[] = {1, 0x0b, 0x20, 7, 1, 0x60, 0, 0x30, 0, 0, 0};
static const uint8_t scan_enable[] = {1, 0x0c, 0x20, 2, 1, 0};
static const uint8_t scan_disable[] = {1, 0x0c, 0x20, 2, 0, 0};

static int mock_send(void* context, const uint8_t* data, size_t size) {
    MockH4* mock = context;
    mock->attempts++;
    if (mock->send_result != BT_OK) return mock->send_result;
    CHECK(size <= sizeof(mock->tx));
    memcpy(mock->tx, data, size);
    mock->tx_size = size;
    mock->sends++;
    return BT_OK;
}

static int mock_receive(void* context, uint8_t* byte) {
    MockH4* mock = context;
    if (mock->receive_error) return mock->receive_error;
    if (mock->rx_used == mock->rx_size) return 0;
    *byte = mock->rx[mock->rx_used++];
    return 1;
}

static uint32_t mock_time(void* context) {
    return ((MockH4*)context)->now;
}

static void queue(Fixture* f, const uint8_t* data, size_t size) {
    MockH4* mock = &f->mock;
    if (mock->rx_used == mock->rx_size) mock->rx_used = mock->rx_size = 0;
    CHECK(size <= sizeof(mock->rx) - mock->rx_size);
    memcpy(mock->rx + mock->rx_size, data, size);
    mock->rx_size += size;
}

static void complete(Fixture* f, uint16_t opcode, uint8_t credits, uint8_t result) {
    const uint8_t event[] = {4, 0x0e, 4, credits, (uint8_t)opcode, (uint8_t)(opcode >> 8), result};
    queue(f, event, sizeof(event));
    bt_hci_poll(&f->hci);
}

static void expect_state(const Fixture* f, uint32_t state, uint32_t count, int32_t error) {
    BtStatus status;
    bt_hci_status(&f->hci, &status);
    CHECK(status.state == state && status.count == count && status.error == error);
    CHECK(status.reserved == 0);
}

static void expect_command(const Fixture* f, unsigned int sends, const uint8_t* data, size_t size) {
    CHECK(f->mock.sends == sends && f->mock.tx_size == size);
    CHECK(memcmp(f->mock.tx, data, size) == 0);
}

static void init(Fixture* f, uint32_t now) {
    memset(f, 0, sizeof(*f));
    f->mock.now = now;
    f->mock.send_result = BT_OK;
    BtH4Io io = {&f->mock, mock_send, mock_receive, mock_time};
    CHECK(bt_hci_init(&f->hci, &io) == BT_OK);
    expect_state(f, BT_BOOTING, 0, 0);
    CHECK(f->mock.attempts == 0);
}

static void boot(Fixture* f, uint32_t now) {
    init(f, now);
    CHECK(bt_hci_scan_start(&f->hci) == BT_BUSY);
    bt_hci_poll(&f->hci);
    expect_command(f, 1, reset, sizeof(reset));
    complete(f, 0x0c03, 1, 0);
    expect_command(f, 2, event_mask, sizeof(event_mask));
    complete(f, 0x0c01, 1, 0);
    expect_command(f, 3, le_event_mask, sizeof(le_event_mask));
    complete(f, 0x2001, 1, 0);
    expect_state(f, BT_READY, 0, 0);
}

static void prepare_scan(Fixture* f) {
    CHECK(bt_hci_scan_start(&f->hci) == BT_OK);
    CHECK(bt_hci_scan_start(&f->hci) == BT_BUSY);
    expect_state(f, BT_SCANNING, 0, 0);
    bt_hci_poll(&f->hci);
    expect_command(f, 4, scan_parameters, sizeof(scan_parameters));
    complete(f, 0x200b, 1, 0);
    expect_command(f, 5, scan_enable, sizeof(scan_enable));
}

static void report(Fixture* f, uint8_t event_type, uint8_t address_type, uint8_t address_byte,
                   const uint8_t* data, uint8_t size, uint8_t rssi) {
    uint8_t event[3 + 2 + 10 + 31] = {
        4, 0x3e, (uint8_t)(12 + size), 2, 1,
        event_type, address_type, address_byte, 2, 3, 4, 5, 6, size
    };
    CHECK(size <= 31);
    if (size) memcpy(event + 14, data, size);
    event[14 + size] = rssi;
    queue(f, event, (size_t)15 + size);
    bt_hci_poll(&f->hci);
}

static void test_scan(void) {
    Fixture f;
    BtDevice device = {0};
    const uint8_t reports[] = {
        4, 0x3e, 27, 2, 2,
        0, 0, 1, 2, 3, 4, 5, 6, 5, 4, 8, 'P', 'i', '4', 0xb9,
        0, 1, 1, 2, 3, 4, 5, 6, 0, 0x7f
    };
    const uint8_t address[] = {1, 2, 3, 4, 5, 6};
    const uint8_t name[] = {7, 9, 'P', 'i', ' ', 'F', 'o', 'u'};
    const uint8_t shortened[] = {2, 8, 'P'};
    const uint8_t bad_data[] = {5, 9, 'B', 'a', 'i', 't', 3, 1};
    const uint8_t bad_reports[] = {
        4, 0x3e, 13, 2, 2, 0, 0, 0x33, 2, 3, 4, 5, 6, 0, 0xd0, 0
    };
    const uint8_t missing_rssi[] = {4, 0x3e, 11, 2, 1, 0, 0, 8, 2, 3, 4, 5, 6, 0};

    boot(&f, UINT32_MAX - 1000000U);
    CHECK(bt_hci_device(&f.hci, 0, &device) == 0);
    prepare_scan(&f);
    queue(&f, reports, sizeof(reports));
    bt_hci_poll(&f.hci);
    expect_state(&f, BT_SCANNING, 0, 0);
    f.mock.now += 20000U;
    complete(&f, 0x200c, 1, 0);

    queue(&f, reports, 11);
    bt_hci_poll(&f.hci);
    expect_state(&f, BT_SCANNING, 0, 0);
    f.mock.now += 1000U;
    queue(&f, reports + 11, sizeof(reports) - 11);
    bt_hci_poll(&f.hci);
    expect_state(&f, BT_SCANNING, 2, 0);
    CHECK(bt_hci_device(&f.hci, 0, &device) == BT_OK);
    CHECK(memcmp(device.address, address, sizeof(address)) == 0 && device.address_type == 0);
    CHECK(device.rssi == -71 && device.reserved == 0 && strcmp(device.name, "Pi4") == 0);
    CHECK(bt_hci_device(&f.hci, 1, &device) == BT_OK);
    CHECK(memcmp(device.address, address, sizeof(address)) == 0 && device.address_type == 1);
    CHECK(device.rssi == 127 && !device.name[0]);
    CHECK(bt_hci_device(&f.hci, 2, &device) == 0 && device.address_type == 1);

    report(&f, 4, 0, 1, name, sizeof(name), 0xc0);
    report(&f, 0, 0, 1, shortened, sizeof(shortened), 0xc1);
    report(&f, 0, 0, 1, bad_data, sizeof(bad_data), 0xc2);
    queue(&f, bad_reports, sizeof(bad_reports));
    queue(&f, missing_rssi, sizeof(missing_rssi));
    bt_hci_poll(&f.hci);
    expect_state(&f, BT_SCANNING, 2, 0);
    CHECK(bt_hci_device(&f.hci, 0, &device) == BT_OK);
    CHECK(device.rssi == -62 && strcmp(device.name, "Pi Fou") == 0);

    f.mock.now += 4998999U;
    bt_hci_poll(&f.hci);
    CHECK(f.mock.sends == 5);
    f.mock.now++;
    report(&f, 0, 0, 3, NULL, 0, 0xd0);
    expect_command(&f, 6, scan_disable, sizeof(scan_disable));
    expect_state(&f, BT_SCANNING, 2, 0);
    CHECK(bt_hci_scan_start(&f.hci) == BT_BUSY);
    complete(&f, 0x200c, 1, 0);
    expect_state(&f, BT_READY, 2, 0);
    CHECK(bt_hci_device(&f.hci, 0, &device) == BT_OK);
    CHECK(bt_hci_scan_start(&f.hci) == BT_OK);
    expect_state(&f, BT_SCANNING, 0, 0);
    CHECK(bt_hci_device(&f.hci, 0, &device) == 0);
}

static void test_result_limit(void) {
    Fixture f;
    BtDevice device;
    uint8_t long_name[31] = {30, 9};
    memset(long_name + 2, 'A', sizeof(long_name) - 2);

    boot(&f, 0);
    prepare_scan(&f);
    complete(&f, 0x200c, 1, 0);
    for (uint8_t i = 0; i <= BT_DEVICE_LIMIT; i++) report(&f, 0, 0, i, NULL, 0, 0xe0);
    expect_state(&f, BT_SCANNING, BT_DEVICE_LIMIT, 0);
    CHECK(bt_hci_device(&f.hci, BT_DEVICE_LIMIT - 1U, &device) == BT_OK);
    CHECK(device.address[0] == BT_DEVICE_LIMIT - 1U);
    CHECK(bt_hci_device(&f.hci, BT_DEVICE_LIMIT, &device) == BT_PROTOCOL);

    report(&f, 4, 0, 0, long_name, sizeof(long_name), 0x81);
    CHECK(bt_hci_device(&f.hci, 0, &device) == BT_OK);
    CHECK(device.rssi == -127 && strlen(device.name) == 29);
    CHECK(memcmp(device.name, long_name + 2, 29) == 0);
}

static void test_command_flow(void) {
    Fixture f;
    const uint8_t command_pending[] = {4, 0x0f, 4, 0, 1, 3, 0x0c};
    const uint8_t grant_credit[] = {4, 0x0e, 3, 1, 0, 0};

    init(&f, 0);
    f.mock.send_result = BT_BUSY;
    bt_hci_poll(&f.hci);
    CHECK(f.mock.attempts == 1 && f.mock.sends == 0);
    f.mock.send_result = BT_OK;
    bt_hci_poll(&f.hci);
    expect_command(&f, 1, reset, sizeof(reset));
    queue(&f, command_pending, sizeof(command_pending));
    complete(&f, 0x0c01, 1, 0);
    expect_state(&f, BT_BOOTING, 0, 0);
    CHECK(f.mock.attempts == 2);

    complete(&f, 0x0c03, 0, 0);
    CHECK(f.mock.attempts == 2);
    bt_hci_poll(&f.hci);
    CHECK(f.mock.attempts == 2);
    queue(&f, grant_credit, sizeof(grant_credit));
    bt_hci_poll(&f.hci);
    expect_command(&f, 2, event_mask, sizeof(event_mask));
}

static void test_receive_budget(void) {
    Fixture f;
    const uint8_t large_event[258] = {4, 0xff, 255};
    const uint8_t unexpected_data[] = {
        2, 1, 0, 7, 0, 4, 0x0e, 4, 1, 3, 0x0c, 0,
        3, 1, 0, 1, 0xff,
        5, 1, 0, 1, 0x80, 0xff
    };

    init(&f, 0);
    bt_hci_poll(&f.hci);
    queue(&f, large_event, sizeof(large_event));
    queue(&f, unexpected_data, sizeof(unexpected_data));
    bt_hci_poll(&f.hci);
    CHECK(f.mock.rx_used > 0 && f.mock.rx_used < f.mock.rx_size);
    CHECK(f.mock.sends == 1);
    complete(&f, 0x0c03, 1, 0);
    CHECK(f.mock.rx_used == f.mock.rx_size);
    expect_command(&f, 2, event_mask, sizeof(event_mask));
}

static void test_timeouts(void) {
    Fixture f;
    BtDevice device;
    const uint8_t partial_event[] = {4, 0x3e, 12, 2, 1};

    init(&f, UINT32_MAX - 100);
    bt_hci_poll(&f.hci);
    f.mock.now += 999999U;
    complete(&f, 0x2001, 1, 0);
    expect_state(&f, BT_BOOTING, 0, 0);
    f.mock.now++;
    bt_hci_poll(&f.hci);
    expect_state(&f, BT_FAILED, 0, BT_TIMEOUT);
    CHECK(bt_hci_scan_start(&f.hci) == BT_TIMEOUT);
    CHECK(bt_hci_device(&f.hci, 0, &device) == BT_TIMEOUT);
    complete(&f, 0x0c03, 1, 0);
    expect_state(&f, BT_FAILED, 0, BT_TIMEOUT);
    CHECK(f.mock.sends == 1);

    init(&f, 0);
    f.mock.send_result = BT_BUSY;
    bt_hci_poll(&f.hci);
    f.mock.now += 1000000U;
    bt_hci_poll(&f.hci);
    expect_state(&f, BT_FAILED, 0, BT_TIMEOUT);
    CHECK(f.mock.attempts == 1 && f.mock.sends == 0);

    boot(&f, 0);
    prepare_scan(&f);
    complete(&f, 0x200c, 1, 0);
    report(&f, 0, 0, 1, NULL, 0, 0xd0);
    queue(&f, partial_event, sizeof(partial_event));
    bt_hci_poll(&f.hci);
    f.mock.now += 249999U;
    bt_hci_poll(&f.hci);
    expect_state(&f, BT_SCANNING, 1, 0);
    f.mock.now++;
    bt_hci_poll(&f.hci);
    expect_state(&f, BT_FAILED, 0, BT_TIMEOUT);

    boot(&f, 0);
    prepare_scan(&f);
    complete(&f, 0x200c, 1, 0);
    f.mock.now += 5000000U;
    bt_hci_poll(&f.hci);
    expect_command(&f, 6, scan_disable, sizeof(scan_disable));
    f.mock.now += 1000000U;
    bt_hci_poll(&f.hci);
    expect_state(&f, BT_FAILED, 0, BT_TIMEOUT);
}

static void test_errors(void) {
    Fixture f = {0};
    BtDevice device;
    const uint8_t bad_indicator[] = {0x99};
    const uint8_t missing_status[] = {4, 0x0e, 3, 1, 3, 0x0c};
    const uint8_t hardware_error[] = {4, 0x10, 1, 0x01};

    expect_state(&f, BT_DISABLED, 0, 0);
    CHECK(bt_hci_scan_start(&f.hci) == BT_UNAVAILABLE);
    CHECK(bt_hci_device(&f.hci, 0, &device) == BT_UNAVAILABLE);
    CHECK(bt_hci_device(&f.hci, 0, NULL) == BT_PROTOCOL);
    CHECK(bt_hci_init(&f.hci, NULL) == BT_UNAVAILABLE);
    expect_state(&f, BT_DISABLED, 0, BT_UNAVAILABLE);

    init(&f, 0);
    f.mock.send_result = BT_TIMEOUT;
    bt_hci_poll(&f.hci);
    expect_state(&f, BT_FAILED, 0, BT_TIMEOUT);

    init(&f, 0);
    f.mock.receive_error = BT_IO;
    bt_hci_poll(&f.hci);
    expect_state(&f, BT_FAILED, 0, BT_IO);
    CHECK(f.mock.sends == 0);

    init(&f, 0);
    queue(&f, bad_indicator, sizeof(bad_indicator));
    bt_hci_poll(&f.hci);
    expect_state(&f, BT_FAILED, 0, BT_PROTOCOL);

    init(&f, 0);
    bt_hci_poll(&f.hci);
    queue(&f, missing_status, sizeof(missing_status));
    bt_hci_poll(&f.hci);
    expect_state(&f, BT_FAILED, 0, BT_PROTOCOL);

    init(&f, 0);
    bt_hci_poll(&f.hci);
    complete(&f, 0x0c03, 1, 0x0c);
    expect_state(&f, BT_FAILED, 0, BT_PROTOCOL);

    boot(&f, 0);
    queue(&f, hardware_error, sizeof(hardware_error));
    bt_hci_poll(&f.hci);
    expect_state(&f, BT_FAILED, 0, BT_IO);
}

int main(void) {
    test_scan();
    test_result_limit();
    test_command_flow();
    test_receive_budget();
    test_timeouts();
    test_errors();
    return 0;
}
