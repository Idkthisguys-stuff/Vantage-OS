#include "../include/user_lib.h"
#include "../../includes/task_api.h"

#ifdef VANTAGE_SIM
/* Desktop builds use the same dispatcher without an ARM exception. */
uint64_t handle_sys_call(uint64_t syscall_id, uint64_t arg0);

static inline uint64_t syscall(uint64_t id, uint64_t arg) {
    return handle_sys_call(id, arg);
}
#else
#define SIMD_CLOBBERS "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", \
                      "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", \
                      "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31"

static inline uint64_t syscall(uint64_t id, uint64_t arg) {
    register uint64_t r0 __asm__("x0") = arg;
    register uint64_t r8 __asm__("x8") = id;

    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r8) : "memory", SIMD_CLOBBERS);

    return r0;
}
#endif

uint64_t sys_task_id(void) {
    return syscall(SYS_TASK_ID, 0);
}

uint32_t sys_task_yield(void) {
    return (uint32_t)syscall(SYS_TASK_YIELD, 0);
}

uint32_t sys_task_exit(int status) {
    return (uint32_t)syscall(SYS_TASK_EXIT, (uint32_t)status);
}

static int copy_path(char dest[FILE_PATH_MAX], const char* path) {
    if (!path) return 0;
    for (unsigned int i = 0; i < FILE_PATH_MAX; i++) {
        dest[i] = path[i];
        if (!path[i]) return 1;
    }
    return 0;
}

int64_t sys_file_open(const char* path, uint32_t flags) {
    FileOpen request = {0};
    if (!copy_path(request.path, path)) return FILE_BAD;
    request.flags = flags;
    return (int64_t)syscall(SYS_FILE_OPEN, (uintptr_t)&request);
}

int sys_file_read(uint64_t handle, void* data, uint32_t length) {
    FileIo request = {0};
    if (length > FILE_IO_MAX || (length && !data)) return FILE_BAD;
    request.handle = handle;
    request.length = length;
    int result = (int)syscall(SYS_FILE_READ, (uintptr_t)&request);
    if (result > (int)length) return FILE_BAD;
    for (int i = 0; i < result; i++) ((unsigned char*)data)[i] = request.data[i];
    return result;
}

int sys_file_write(uint64_t handle, const void* data, uint32_t length) {
    FileIo request = {0};
    if (length > FILE_IO_MAX || (length && !data)) return FILE_BAD;
    request.handle = handle;
    request.length = length;
    for (unsigned int i = 0; i < length; i++) request.data[i] = ((const unsigned char*)data)[i];
    return (int)syscall(SYS_FILE_WRITE, (uintptr_t)&request);
}

int sys_file_seek(uint64_t handle, uint32_t offset) {
    FileSeek request = {handle, offset, 0};
    return (int)syscall(SYS_FILE_SEEK, (uintptr_t)&request);
}

int sys_file_close(uint64_t handle) {
    return (int)syscall(SYS_FILE_CLOSE, handle);
}

int sys_file_list(FileEntry* entry) {
    return (int)syscall(SYS_FILE_LIST, (uintptr_t)entry);
}

int sys_file_remove(const char* path) {
    FilePath request = {{0}};
    if (!copy_path(request.path, path)) return FILE_BAD;
    return (int)syscall(SYS_FILE_REMOVE, (uintptr_t)&request);
}

uint32_t sys_touch_read(TouchEvent* event) {
    return (uint32_t)syscall(1, (uint64_t)event);
}

void sys_canvas_set_color(uint32_t color) {
    syscall(2, color);
}

void sys_canvas_clear(void) {
    syscall(3, 0);
}

void sys_canvas_fill_circle(int x, int y, int r) {
    struct circle_args args = { .x = x, .y = y, .r = r };
    syscall(4, (uint64_t)&args);
}

void sys_canvas_present(void) {
    syscall(5, 0);
}
