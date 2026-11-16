#include <vantage/kernel/render.h>

static void enter(Renderer* renderer) {
    if (renderer->busy) parallel_halt("[ERR] Renderer re-entered; system halted\n");
    renderer->busy = 1;
}

static void cpu_fill(RasterFill* fill) {
    /* Keep tiny spans local; full-canvas clears use all four cores. */
    if (fill->height >= PARALLEL_CORES && (uint64_t)fill->width * fill->height >= 4096) {
        if (!parallel_run(raster_fill_rows, fill))
            parallel_halt("[ERR] Parallel fill submission failed; system halted\n");
    } else raster_fill_rows(0, 1, fill);
}

static void cpu_present(RasterCopy* copy) {
    if (!parallel_run(raster_copy_rows, copy))
        parallel_halt("[ERR] Parallel frame copy failed; system halted\n");
}

/* Preserve only a surface the backend has returned for CPU reads. A failed
 * whole-surface clear can instead discard it and refill the CPU fallback. */
static void retire(Renderer* renderer, int preserve) {
    const RenderBackend* failed = renderer->backend;
    if (preserve) memcpy(renderer->fallback, failed->surface,
                         (size_t)renderer->width * renderer->height * sizeof(uint32_t));
    renderer->pixels = renderer->fallback;
    renderer->backend = NULL;
    if (failed->retired) failed->retired(failed->context);
}

void render_init(Renderer* renderer, uint32_t* pixels, uint32_t width, uint32_t height) {
    renderer->pixels = renderer->fallback = pixels;
    renderer->width = width;
    renderer->height = height;
    renderer->backend = NULL;
    renderer->busy = 0;
}

uint32_t render_attach(Renderer* renderer, const RenderBackend* backend) {
    if (!renderer->fallback || renderer->backend || !backend || !backend->surface) return 0;
    enter(renderer);
    memcpy(backend->surface, renderer->pixels,
           (size_t)renderer->width * renderer->height * sizeof(uint32_t));
    renderer->pixels = backend->surface;
    renderer->backend = backend;
    renderer->busy = 0;
    return 1;
}

void render_pixel(Renderer* renderer, uint32_t x, uint32_t y, uint32_t color) {
    if (x >= renderer->width || y >= renderer->height) return;
    enter(renderer);
    renderer->pixels[(size_t)y * renderer->width + x] = color;
    renderer->busy = 0;
}

void render_fill(Renderer* renderer, uint32_t x, uint32_t y,
                 uint32_t width, uint32_t height, uint32_t color) {
    if (!width || !height || x >= renderer->width || y >= renderer->height ||
        width > renderer->width - x || height > renderer->height - y) return;
    enter(renderer);
    RasterFill fill = { renderer->pixels, renderer->width, x, y, width, height, color };
    int result = renderer->backend && renderer->backend->fill
        ? renderer->backend->fill(renderer->backend->context, &fill) : RENDER_SKIP;
    if (result == RENDER_FAILED || result == RENDER_DISCARD) {
        if (result == RENDER_DISCARD && (x || y || width != renderer->width || height != renderer->height))
            parallel_halt("[ERR] Backend discarded a partial fill; system halted\n");
        retire(renderer, result != RENDER_DISCARD);
        fill.pixels = renderer->pixels;
    }
    if (result != RENDER_DONE) cpu_fill(&fill);
    renderer->busy = 0;
}

uint32_t render_present(Renderer* renderer, volatile uint32_t* destination,
                        uint32_t destination_pitch) {
    if (!destination || destination_pitch < renderer->width) return 0;
    enter(renderer);
    RasterCopy copy = { renderer->pixels, destination, renderer->width,
                        destination_pitch, renderer->width, renderer->height };
    int result = renderer->backend && renderer->backend->present
        ? renderer->backend->present(renderer->backend->context, &copy) : RENDER_SKIP;
    if (result == RENDER_FAILED || result == RENDER_TARGET_LOST) {
        retire(renderer, 1);
        if (result == RENDER_TARGET_LOST) {
            renderer->busy = 0;
            return 0;
        }
        copy.source = renderer->pixels;
    }
    if (result != RENDER_DONE) cpu_present(&copy);
    renderer->busy = 0;
    return 1;
}
