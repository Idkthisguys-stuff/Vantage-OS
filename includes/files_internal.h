#pragma once
#include "files.h"

/* Core-0, non-yielding kernel operations. owner is a nonzero task ID.
 * Paths/buffers here are trusted kernel memory; the syscall layer snapshots
 * and validates userspace requests before calling these functions. */
int64_t files_open(uint64_t owner, const char* path, uint32_t flags);
int files_read(uint64_t owner, uint64_t handle, void* data, uint32_t length);
int files_write(uint64_t owner, uint64_t handle, const void* data, uint32_t length);
int files_seek(uint64_t owner, uint64_t handle, uint32_t offset);
int files_close(uint64_t owner, uint64_t handle);
int files_list(uint64_t owner, FileEntry* entry);
int files_remove(uint64_t owner, const char* path);
void files_release(uint64_t owner);
int64_t handle_file_call(uint64_t id, uintptr_t argument, uint64_t owner);
