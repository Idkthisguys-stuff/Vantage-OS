// I had AI to assist me since there was lack of documentation and tutorials :/
/* There're governments funding billions on AI research
   and then theres people who dedicate their lives to hate on it.
   Don't listen to those peasants. They are nothing more than uneducated fools. 
   How dare one challenge a perfect immortal machine?                           */

#include <vantage/platform/pi4/display.h>
#include <vantage/arch/mmu.h>
#include <vantage/kernel/render.h>
#include <vantage/kernel/memory.h>

#include <vantage/platform/pi4/io.h>

/* Whole cache lines, exclusively owned by firmware after a submitted timeout. */
static volatile uint32_t mbox[48] __attribute__((aligned(64)));
enum MailboxStatus { MBOX_READY, MBOX_NOT_SUBMITTED, MBOX_QUARANTINED, MBOX_FAILED };
static enum MailboxStatus mbox_status;
static uint32_t display_frozen;

static uint32_t* fbBase;
static uint32_t fbPitch;

static uint32_t pageHeight;
static uint32_t drawPage;
static uint32_t visiblePage;
static uint32_t quarantinedPages;
static uint32_t lastSwap;

#define PAGE_COUNT 3
#define BACK_PIXELS (800 * 480)
#define FRAME_US 16667
#define MBOX_TIMEOUT_US 1000000U
#define MBOX_POLL_LIMIT 8000000U

/* CPU-owned draw surface. It stays allocated while an accelerated backend
 * draws into its own surface, so a runtime fallback never has to allocate. */
static uint32_t* backBuffer;
static Renderer renderer;

uint32_t fb_attach_backend(const RenderBackend* backend) {
    if (!backBuffer || !backend) return 0;
    return render_attach(&renderer, backend);
}

static void select_draw_page(void) {
    for (unsigned int i = 0; i < PAGE_COUNT; i++) {
        if (drawPage != visiblePage && !(quarantinedPages & (1U << drawPage))) return;
        drawPage = (drawPage + 1) % PAGE_COUNT;
    }
    mm_halt("[ERR] No safe hidden framebuffer page remains\n");
}

void fb_quarantine_page(void* page) {
    uintptr_t start = (uintptr_t)fbBase, address = (uintptr_t)page;
    uint64_t bytes = (uint64_t)pageHeight * fbPitch;
    if (!backBuffer || !bytes || address < start || address - start >= bytes * PAGE_COUNT ||
        (address - start) % bytes)
        mm_halt("[ERR] Invalid framebuffer page quarantine\n");
    uint32_t index = (uint32_t)((address - start) / bytes);
    if (index == visiblePage) mm_halt("[ERR] Device still owns the visible framebuffer page\n");
    if (!(quarantinedPages & (1U << index)))
        uart_writeText("[WARN] Framebuffer page quarantined; CPU display uses the remaining pages\n");
    quarantinedPages |= 1U << index;
}

void* fb_hidden_page(void) {
    if (display_frozen || !backBuffer || fbPitch != renderer.width * 4) return NULL;
    select_draw_page();
    return (void*)((uintptr_t)fbBase + drawPage * pageHeight * fbPitch);
}

static void cache_mailbox(volatile void* addr, uint32_t size, uint32_t invalidate) {
    uintptr_t start = (uintptr_t)addr & ~63ULL;
    uintptr_t end = (uintptr_t)addr + size;

    for (uintptr_t p = start; p < end; p += 64) {
        if (invalidate) __asm__ volatile("dc ivac, %0" :: "r"(p) : "memory");
        else __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

uint32_t time_us(void) {
    return *(volatile uint32_t*)TIMER_CLO;
}

uint32_t mbox_call(uint8_t ch) {
    volatile uint32_t* status = (volatile uint32_t*)MBOX_STATUS;
    volatile uint32_t* read = (volatile uint32_t*)MBOX_READ;
    volatile uint32_t* write = (volatile uint32_t*)MBOX_WRITE;

    uint32_t msg = ((uint32_t)((uintptr_t)mbox) & ~0xF) | (ch & 0xF);

    if (mbox_status == MBOX_QUARANTINED) return 0;
    cache_mailbox(mbox, sizeof(mbox), 0);
    uint32_t start = time_us();
    unsigned int polls;
    uint32_t ready = 0;
    for (polls = 0; polls < MBOX_POLL_LIMIT; polls++) {
        if (!(*status & MBOX_FULL)) {
            ready = 1;
            break;
        }
        if ((uint32_t)(time_us() - start) >= MBOX_TIMEOUT_US) break;
    }
    if (!ready) {
        mbox_status = MBOX_NOT_SUBMITTED;
        return 0;
    }
    *write = msg;
    start = time_us();
    for (polls = 0; polls < MBOX_POLL_LIMIT; polls++) {
        if (!(*status & MBOX_EMPTY) && *read == msg) {
            cache_mailbox(mbox, sizeof(mbox), 1);
            mbox_status = mbox[1] == MBOX_RESPONSE ? MBOX_READY : MBOX_FAILED;
            return mbox_status == MBOX_READY;
        }
        if ((uint32_t)(time_us() - start) >= MBOX_TIMEOUT_US) break;
    }
    mbox_status = MBOX_QUARANTINED;
    return 0;
}

static int tag_response(unsigned int index, uint32_t tag, uint32_t bytes) {
    return mbox[index] == tag && mbox[index + 1] == bytes &&
           mbox[index + 2] == (MBOX_RESPONSE | bytes);
}

uint32_t* fb_init(uint32_t width, uint32_t height) {
    if (!width || !height || (uint64_t)width * height > BACK_PIXELS || (width & 1) || backBuffer) {
        mm_halt("[ERR] Invalid or repeated framebuffer initialization\n");
    }

    mbox[0] = 39 * 4;
    mbox[1] = 0;

    mbox[2] = 0x00048003;
    mbox[3] = 8;
    mbox[4] = 0;
    mbox[5] = width;
    mbox[6] = height;

    mbox[7] = 0x00048004;
    mbox[8] = 8;
    mbox[9] = 0;
    mbox[10] = width;
    mbox[11] = height * 3;

    mbox[12] = 0x00048009;
    mbox[13] = 8;
    mbox[14] = 0;
    mbox[15] = 0;
    mbox[16] = 0;

    mbox[17] = 0x00048005;
    mbox[18] = 4;
    mbox[19] = 0;
    mbox[20] = 32;

    mbox[21] = 0x00048006;
    mbox[22] = 4;
    mbox[23] = 0;
    mbox[24] = 0;

    mbox[25] = 0x00040001;
    mbox[26] = 8;
    mbox[27] = 0;
    mbox[28] = 4096;
    mbox[29] = 0;

    mbox[30] = 0x00040008;
    mbox[31] = 4;
    mbox[32] = 0;
    mbox[33] = 0;

    mbox[34] = 0x00048007;
    mbox[35] = 4;
    mbox[36] = 0;
    mbox[37] = 2;

    mbox[38] = 0;

    if (!mbox_call(MBOX_CH_PROP) ||
        !tag_response(2, 0x00048003, 8) || mbox[5] != width || mbox[6] != height ||
        !tag_response(7, 0x00048004, 8) || mbox[10] != width || mbox[11] != height * PAGE_COUNT ||
        !tag_response(12, 0x00048009, 8) || mbox[15] || mbox[16] || mbox[20] != 32 || mbox[28] == 0) {
        mm_halt("[ERR] Firmware framebuffer request failed\n");
    }
    if (mbox[33] < width * 4 || (mbox[33] & 3) ||
        (uint64_t)height * PAGE_COUNT * mbox[33] > mbox[29])
        mm_halt("[ERR] Invalid firmware framebuffer allocation\n");

    fbBase = (uint32_t*)(uintptr_t)(mbox[28] & 0x3FFFFFFF);
    fbPitch = mbox[33];
    pageHeight = height;
    drawPage = 1;
    visiblePage = quarantinedPages = 0;
    lastSwap = time_us();

    if (!mm_finalize_framebuffer((uintptr_t)fbBase, mbox[29]))
        mm_halt("[ERR] Firmware memory reservations conflict; boot halted\n");
    mmu_init((uintptr_t)fbBase, mbox[29]);
    if (!mm_init_allocator() || !mm_self_test())
        mm_halt("[ERR] Memory allocator initialization/self-test failed\n");
    uart_writeText("[OK] Page allocator and heap self-test passed\n");
    /* Keep the old capacity used by the panel's fixed coordinate transform. */
    backBuffer = mm_zalloc((uint64_t)BACK_PIXELS * sizeof(uint32_t), 64);
    if (!backBuffer) mm_halt("[ERR] Back-buffer allocation failed\n");
    render_init(&renderer, backBuffer, width, height);

    uart_writeText("[OK] Vantage display driver...\n");
    return fbBase;
}

void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    render_pixel(&renderer, y, 480 - 1 - x, color);
}

void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    render_fill(&renderer, y, 480 - x - w, h, w, color);
}

void fb_wait_frame(void) {
    while (time_us() - lastSwap < FRAME_US);
    lastSwap = time_us();
}

static void freeze_display(void) {
    if (!display_frozen)
        uart_writeText("[ERR] Framebuffer flip unconfirmed; scanout updates stopped until reboot\n");
    display_frozen = 1;
}

void fb_swap(void) {
    if (display_frozen || mbox_status == MBOX_QUARANTINED) return;
    select_draw_page();
    uintptr_t page = (uintptr_t)fbBase + drawPage * pageHeight * fbPitch;
    if (!render_present(&renderer, (volatile uint32_t*)page, fbPitch / 4)) {
        /* The renderer retired the device but did not touch its lost target.
         * Keep that page reserved and retry the intact frame on a safe page. */
        fb_quarantine_page((void*)page);
        select_draw_page();
        page = (uintptr_t)fbBase + drawPage * pageHeight * fbPitch;
        if (!render_present(&renderer, (volatile uint32_t*)page, fbPitch / 4))
            mm_halt("[ERR] CPU framebuffer retry failed\n");
    }

    __asm__ volatile("dsb sy" ::: "memory");

    mbox[0] = 8 * 4;
    mbox[1] = 0;
    mbox[2] = 0x00048009;
    mbox[3] = 8;
    mbox[4] = 0;
    mbox[5] = 0;
    mbox[6] = drawPage * pageHeight;
    mbox[7] = 0;
    if (!mbox_call(MBOX_CH_PROP)) {
        if (mbox_status != MBOX_NOT_SUBMITTED) freeze_display();
        return;
    }
    if (mbox[0] != 8 * 4 || !tag_response(2, 0x00048009, 8) ||
        mbox[5] != 0 || mbox[6] != drawPage * pageHeight || mbox[7] != 0) {
        freeze_display();
        return;
    }
    visiblePage = drawPage;
    drawPage = (drawPage + 1) % PAGE_COUNT;
}
