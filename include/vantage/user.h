#pragma once
#include <vantage/abi/files.h>
#include <vantage/abi/graphics.h>
#include <vantage/abi/touch.h>
#include <vantage/abi/console.h>
#include <vantage/abi/net.h>

/* Persistent /ram files. Transfers are bounded to FILE_IO_MAX bytes per call. */
int64_t sys_file_open(const char* path, uint32_t flags);
int sys_file_read(uint64_t handle, void* data, uint32_t length);
int sys_file_write(uint64_t handle, const void* data, uint32_t length);
int sys_file_seek(uint64_t handle, uint32_t offset);
int sys_file_close(uint64_t handle);
int sys_file_list(FileEntry* entry);
int sys_file_remove(const char* path);
int sys_file_directory(FileDirectory* request);
int sys_file_directory_long(FileDirectoryLong* request);

/* Wi-Fi, Ethernet and IPv4 networking. Calls never block: NET_AGAIN means
 * retry later (for example after sys_frame_wait, which also services the
 * network). */
int sys_net_status(NetStatus* status);
int sys_net_scan(NetScan* request);
/* A NULL or empty passphrase selects an open network explicitly. flags:
 * NET_JOIN_HIDDEN. The copy passed to the kernel is erased after the call. */
int sys_net_join(const char* ssid, const char* passphrase, uint32_t flags);
int sys_net_leave(void);
/* The request is erased after the call, including any passphrase. */
int sys_net_profile(NetProfile* request);
int sys_net_time(NetTime* time);
/* Fills data with hardware entropy: length, or NET_UNAVAILABLE without a
 * strong source. */
int sys_net_random(void* data, uint32_t length);
int sys_net_resolve(const char* name, uint32_t* address);
int64_t sys_net_socket(NetSocketCall* call);
int64_t sys_net_open(uint32_t type);
int sys_net_connect(uint64_t handle, uint32_t address, uint16_t port);
int sys_net_send(uint64_t handle, const void* data, uint32_t length);
int sys_net_receive(uint64_t handle, void* data, uint32_t capacity);
int sys_net_close(uint64_t handle);

uint64_t sys_task_id(void);
uint32_t sys_task_yield(void);
/* Returns 0 when scheduling is unavailable; otherwise does not return. */
uint32_t sys_task_exit(int status);

uint32_t sys_touch_read(TouchEvent* event);

// DISPLAY SYSCALLS
void sys_canvas_set_color(uint32_t color);
void sys_canvas_clear(void);
void sys_canvas_fill_circle(int x, int y, int r);
void sys_canvas_present(void);

uint32_t sys_key_read(KeyEvent* event);
uint32_t sys_canvas_text_row(const TextRow* row);
uint32_t sys_canvas_button(const CanvasButton* button);
uint32_t sys_console_info(ConsoleInfo* info);
void sys_frame_wait(void);
uint32_t sys_time_us(void); /* Wrapping monotonic microseconds. */
void sys_console_write(const char* text, size_t length);
