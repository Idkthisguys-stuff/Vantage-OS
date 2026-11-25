#include <vantage/platform/pi4/drivers.h>
#include <vantage/drivers/touch.h>
#include <vantage/drivers/v3d.h>
#include <vantage/platform/pi4/display.h>
#include <vantage/platform/pi4/usb_input.h>
#include <vantage/kernel/memory.h>
#include <vantage/kernel/render.h>
#include <vantage/platform/pi4/io.h>

#ifndef VANTAGE_V3D
#define VANTAGE_V3D 0
#endif
#ifndef VANTAGE_WIFI
#define VANTAGE_WIFI 0
#endif
#ifndef VANTAGE_WIFI_ASSETS
#define VANTAGE_WIFI_ASSETS 0
#endif
#ifndef VANTAGE_ETHERNET
#define VANTAGE_ETHERNET 0
#endif

#if VANTAGE_WIFI && VANTAGE_WIFI_ASSETS
static Sdio sdio;
static Wifi wifi;
extern const WifiFirmware* pi4_wifi_firmware(void);
#endif

#if VANTAGE_V3D || (VANTAGE_WIFI && VANTAGE_WIFI_ASSETS) || VANTAGE_ETHERNET || VANTAGE_USB || VANTAGE_BLUETOOTH
static void report(const char* prefix, int error) {
    char digits[12]; unsigned int used = 0;
    uint32_t value = (uint32_t)(error < 0 ? -(int64_t)error : error);
    do { digits[used++] = (char)('0' + value % 10); value /= 10; } while (value);
    uart_writeText(prefix);
    if (error < 0) uart_writeText("-");
    while (used) { char one[2] = {digits[--used], 0}; uart_writeText(one); }
    uart_writeText(")\n");
}
#endif

#if VANTAGE_RADIO_DIAGNOSTICS && VANTAGE_WIFI && VANTAGE_WIFI_ASSETS
static void wifi_trace(const char* stage, int result, uint32_t firmware_status) {
    uart_writeText("[RADIO] WLAN ");
    uart_writeText(stage);
    uart_writeText(" firmware_status=");
    uart_writeHex(firmware_status);
    report(" result (", result);
}
static void radio_wifi_scan(void) {
    uart_writeText("[RADIO] WLAN scan start\n");
    int result = wifi_scan_start(&wifi);
    if (result == HW_OK) {
        uint32_t start = wifi.bus.time_us(wifi.bus.context);
        while (wifi_scan_status(&wifi) == HW_BUSY &&
               wifi.bus.time_us(wifi.bus.context) - start < 16000000U) {
            result = wifi_poll(&wifi);
            if (result < 0) break;
        }
        if (result >= 0) {
            result = wifi_scan_status(&wifi);
            if (result == HW_BUSY) result = HW_TIMEOUT;
        }
    }
    report("[RADIO] WLAN scan result (", result);
    if (result == HW_OK) report("[RADIO] WLAN access points (", (int)wifi.bss_count);
}
#endif

#if VANTAGE_BLUETOOTH && VANTAGE_RADIO_DIAGNOSTICS
static void radio_bt_scan(void) {
    uart_writeText("[RADIO] BT BLE scan start\n");
    int result = pi4_bt_scan_start();
    if (result == BT_OK) {
        const HwOps* io = pi4_hardware_ops();
        uint32_t start = io->time_us(io->context);
        BtStatus status;
        do {
            pi4_bt_poll();
            pi4_bt_status(&status);
        } while (status.state == BT_SCANNING && io->time_us(io->context) - start < 9000000U);
        if (status.state != BT_READY) result = status.error ? status.error : BT_TIMEOUT;
        else report("[RADIO] BT devices (", (int)status.count);
    }
    report("[RADIO] BT BLE scan result (", result);
}
#endif

#if VANTAGE_V3D
static V3d gpu;
static RenderBackend gpu_backend;
/* Page runs lent to the driver. They are freed once the device is confirmed
 * unable to reach them and stay reserved until reboot otherwise. */
static void* gpu_table;
static void* gpu_arena;

static void gpu_release(void) {
    if (gpu.state == V3D_QUARANTINED) {
        if (gpu.target) fb_quarantine_page(gpu.target);
        uart_writeText("[WARN] V3D stop unconfirmed; its 6 MiB of buffers stay reserved until reboot\n");
        return;
    }
    if (gpu_table) mm_free_pages((uintptr_t)gpu_table, V3D_TABLE_BYTES / 4096);
    if (gpu_arena) mm_free_pages((uintptr_t)gpu_arena, V3D_ARENA_BYTES / 4096);
    gpu_table = gpu_arena = NULL;
}

static void gpu_failure(const char* prefix) {
    uart_writeText(prefix);
    uart_writeText(v3d_stage_name(gpu.stage));
    report(" (", gpu.error);
    if (gpu.detail) {
        uart_writeText("[INFO] V3D_ERR_STAT ");
        uart_writeHex(gpu.detail);
        uart_writeText("\n");
    }
}

/* The renderer has already moved the frame to the CPU surface. */
static void gpu_retired(void* context) {
    (void)context;
    gpu_failure("[ERR] V3D stopped during ");
    gpu_release();
    uart_writeText("[INFO] CPU rendering backend selected; current frame kept\n");
}

static void gpu_start(void) {
    gpu_table = (void*)(uintptr_t)mm_alloc_pages(V3D_TABLE_BYTES / 4096, 16);
    gpu_arena = (void*)(uintptr_t)mm_alloc_pages(V3D_ARENA_BYTES / 4096, 1);
    int result = gpu_table && gpu_arena
        ? v3d_init(&gpu, pi4_hardware_ops(), gpu_table, gpu_arena, 800, 480) : HW_NO_SPACE;
    if (result == HW_OK) result = v3d_self_test(&gpu, fb_hidden_page());
    if (result == HW_OK) {
        v3d_render_backend(&gpu, &gpu_backend);
        gpu_backend.retired = gpu_retired;
        if (!fb_attach_backend(&gpu_backend)) {
            (void)v3d_stop(&gpu);
            result = HW_BUSY;
        }
    }
    if (result == HW_OK) {
        uart_writeText(gpu.present
            ? "[OK] V3D self-test passed; V3D backend selected (clear, present)\n"
            : "[OK] V3D self-test passed; V3D backend selected (clear; CPU present)\n");
        return;
    }
    if (gpu.stage) gpu_failure("[ERR] V3D unavailable: ");
    else report("[ERR] V3D unavailable (", result);
    gpu_release();
    uart_writeText("[INFO] CPU rendering backend selected\n");
}
#endif

#if VANTAGE_ETHERNET
static const NetLink* ethernet_start(void) {
    const NetLink* link = NULL;
    int result = pi4_ethernet_start(pi4_hardware_ops(), &link);
    if (result == HW_OK) {
        static const char hex[] = "0123456789abcdef";
        unsigned char mac[6];
        char text[18];
        link->address(link->context, mac);
        for (unsigned int i = 0; i < 6; i++) {
            text[i * 3] = hex[mac[i] >> 4];
            text[i * 3 + 1] = hex[mac[i] & 15];
            text[i * 3 + 2] = i < 5 ? ':' : 0;
        }
        uart_writeText("[OK] Ethernet ready (MAC ");
        uart_writeText(text);
        uart_writeText(")\n");
    } else if (result == HW_OFF) uart_writeText("[INFO] Ethernet unavailable (no GENET v5 controller)\n");
    else report("[INFO] Ethernet unavailable (", result);
    return link;
}
#endif

#if VANTAGE_USB
static void usb_start(void) {
    Pi4UsbProbe usb;
    int usb_result = pi4_usb_input_start(pi4_hardware_ops(), &usb, TOUCH_HEIGHT, TOUCH_WIDTH);
    if (usb_result == HW_OK) {
        uart_writeText("[OK] VL805 xHCI running: version ");
        uart_writeHex(usb.version);
        uart_writeText(", logical ports ");
        uart_writeHex(usb.ports);
        uart_writeText("\n");
    } else {
        static const char* const stages[] = {
            [PI4_USB_BRIDGE] = "bridge", [PI4_USB_LINK] = "link",
            [PI4_USB_ENDPOINT] = "endpoint", [PI4_USB_BAR] = "BAR",
            [PI4_USB_XHCI] = "xHCI", [PI4_USB_READY] = "probe",
            [PI4_USB_DMA] = "DMA", [PI4_USB_FIRMWARE] = "firmware",
            [PI4_USB_RESET] = "reset", [PI4_USB_NO_OP] = "No Op",
            [PI4_USB_RUNNING] = "root port power"
        };
        uart_writeText("[WARN] VL805 xHCI start failed at ");
        unsigned int stage = (unsigned int)usb.stage;
        uart_writeText(stage < sizeof(stages) / sizeof(stages[0]) && stages[stage]
                       ? stages[stage] : "unknown stage");
        report(" (", usb_result);
        uart_writeText("[INFO] USB controller detail ");
        uart_writeHex(usb.detail);
        uart_writeText("\n");
    }
}
#endif

void pi4_drivers_start(void) {
#if VANTAGE_V3D
    gpu_start();
#else
    uart_writeText("[INFO] V3D disabled at build time (V3D=0); CPU rendering backend selected\n");
#endif
    if (pi4_rng_start(pi4_hardware_ops()) == HW_OK) uart_writeText("[OK] Hardware RNG ready\n");
    else uart_writeText("[WARN] Hardware RNG unavailable; network identifiers use a weak timer-based generator\n");
#if VANTAGE_RADIO_DIAGNOSTICS
    uart_writeText("[RADIO] CA 7 diagnostic batch start\n");
#endif
    const NetLink* wifi_link = NULL;
#if VANTAGE_WIFI && VANTAGE_WIFI_ASSETS
    int wifi_result = sdio_init(&sdio, pi4_hardware_ops());
    if (wifi_result == HW_OK) {
        WifiBus bus = wifi_sdio_bus(&sdio);
#if VANTAGE_RADIO_DIAGNOSTICS
        wifi_result = wifi_boot_traced(&wifi, &bus, pi4_wifi_firmware(), wifi_trace);
#else
        wifi_result = wifi_boot(&wifi, &bus, pi4_wifi_firmware());
#endif
        if (wifi_result == HW_OK) {
            wifi_link = pi4_net_link(&wifi);
            uart_writeText("[OK] CYW43455 firmware ready; use 'net join' to connect\n");
#if VANTAGE_RADIO_DIAGNOSTICS
            radio_wifi_scan();
#endif
        } else {
            report("[ERR] CYW43455 firmware initialization failed (", wifi_result);
            if (wifi.boot_stage) {
                uart_writeText("[INFO] WLAN stage: ");
                uart_writeText(wifi.boot_stage);
                uart_writeText("\n");
            }
            if (wifi.erom_base) {
                uart_writeText("[INFO] WLAN EROM base/bytes read: ");
                uart_writeHex(wifi.erom_base); uart_writeText(" ");
                uart_writeHex(wifi.erom_offset); uart_writeText("\n");
            }
            if (wifi.boot_observed) {
                uart_writeText("[INFO] WLAN startup interrupts/mailbox: ");
                uart_writeHex(wifi.boot_interrupts); uart_writeText(" ");
                uart_writeHex(wifi.boot_mailbox); uart_writeText("\n");
            }
            if (wifi.boot_frame_error) {
                uart_writeText("[INFO] WLAN SDPCM frame rejected: ");
                uart_writeText(wifi.boot_frame_error);
                uart_writeText("\n[INFO] WLAN SDPCM header: ");
                for (unsigned int i = 0; i < 3; i++) {
                    uart_writeHex(wifi.boot_frame_header[i]);
                    uart_writeText(i == 2 ? "\n" : " ");
                }
            }
            if (wifi.boot_ram_tail_valid) {
                uart_writeText("[INFO] WLAN last RAM word: ");
                uart_writeHex(wifi.boot_ram_tail); uart_writeText("\n");
            }
            if (wifi.backplane_failure) {
                uart_writeText("[INFO] WLAN backplane address: ");
                uart_writeHex(wifi.backplane_failure_address);
                uart_writeText("\n");
                report("[INFO] WLAN backplane result (", wifi.backplane_failure);
            }
        }
    } else report("[ERR] WLAN SDIO initialization failed (", wifi_result);
    if (!wifi_link && sdio.failure_command) {
        uart_writeText("[INFO] SDIO command/argument/response/interrupt: ");
        uart_writeHex(sdio.failure_command); uart_writeText(" ");
        uart_writeHex(sdio.failure_argument); uart_writeText(" ");
        uart_writeHex(sdio.failure_response); uart_writeText(" ");
        uart_writeHex(sdio.failure_interrupt); uart_writeText("\n");
    }
#elif VANTAGE_WIFI
    uart_writeText("[INFO] Wi-Fi skipped: supply firmware, NVRAM and CLM via tools/embed_wifi.py\n");
#else
    uart_writeText("[INFO] Wi-Fi disabled at build time (WIFI=0)\n");
#endif
#if VANTAGE_ETHERNET
    const NetLink* eth_link = ethernet_start();
#else
    const NetLink* eth_link = NULL;
    uart_writeText("[INFO] Ethernet disabled at build time (ETHERNET=0)\n");
#endif
    net_attach(wifi_link, eth_link);
#if VANTAGE_USB
    usb_start();
#endif
#if VANTAGE_BLUETOOTH
    int bt_result = pi4_bt_start(pi4_hardware_ops());
    if (bt_result == BT_OK) {
#if VANTAGE_RADIO_DIAGNOSTICS
        uart_writeText("[OK] Bluetooth HCI ready\n");
        radio_bt_scan();
#else
        uart_writeText("[INFO] Bluetooth HCI initialization started\n");
#endif
    } else report("[WARN] Bluetooth initialization failed (", bt_result);
#endif
#if VANTAGE_RADIO_DIAGNOSTICS
    uart_writeText("[RADIO] CA 7 diagnostic batch complete\n");
#endif
}

void pi4_drivers_poll(void) {
    net_poll();
#if VANTAGE_USB
    pi4_usb_input_poll();
    fb_cursor_poll();
#endif
#if VANTAGE_BLUETOOTH
    int bt_result = pi4_bt_poll();
    if (bt_result == BT_OK) uart_writeText("[OK] Bluetooth HCI ready\n");
    else if (bt_result < 0) report("[WARN] Bluetooth HCI operation failed (", bt_result);
#endif
}

Wifi* pi4_wifi_device(void) {
#if VANTAGE_WIFI && VANTAGE_WIFI_ASSETS
    return wifi.state == 1 ? &wifi : NULL;
#else
    return NULL;
#endif
}
