#include <vantage/platform/pi4/drivers.h>
#include <vantage/drivers/v3d.h>
#include <vantage/platform/pi4/display.h>
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

#if VANTAGE_V3D || (VANTAGE_WIFI && VANTAGE_WIFI_ASSETS) || VANTAGE_ETHERNET
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

#if VANTAGE_V3D
static V3d gpu;
static RenderBackend gpu_backend;
/* Page runs lent to the driver. They are freed once the device is confirmed
 * unable to reach them and stay reserved until reboot otherwise. */
static void* gpu_table;
static void* gpu_arena;

static void gpu_release(void) {
    if (gpu.state == V3D_QUARANTINED) {
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

void pi4_drivers_start(void) {
#if VANTAGE_V3D
    gpu_start();
#else
    uart_writeText("[INFO] V3D disabled at build time (V3D=0); CPU rendering backend selected\n");
#endif
    if (pi4_rng_start(pi4_hardware_ops()) == HW_OK) uart_writeText("[OK] Hardware RNG ready\n");
    else uart_writeText("[WARN] Hardware RNG unavailable; network identifiers use a weak timer-based generator\n");
    const NetLink* wifi_link = NULL;
#if VANTAGE_WIFI && VANTAGE_WIFI_ASSETS
    int wifi_result = sdio_init(&sdio, pi4_hardware_ops());
    if (wifi_result == HW_OK) {
        WifiBus bus = wifi_sdio_bus(&sdio);
        wifi_result = wifi_boot(&wifi, &bus, pi4_wifi_firmware());
        if (wifi_result == HW_OK) {
            wifi_link = pi4_net_link(&wifi);
            uart_writeText("[OK] CYW43455 firmware ready; use 'net join' to connect\n");
        } else report("[ERR] CYW43455 firmware initialization failed (", wifi_result);
    } else report("[ERR] WLAN SDIO initialization failed (", wifi_result);
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
}

void pi4_drivers_poll(void) {
    net_poll();
}

Wifi* pi4_wifi_device(void) {
#if VANTAGE_WIFI && VANTAGE_WIFI_ASSETS
    return wifi.state == 1 ? &wifi : NULL;
#else
    return NULL;
#endif
}
