#include <vantage/user.h>
#include <vantage/abi/syscall.h>
#include <stddef.h>

/* The layout refactor must not change existing userspace payloads or numbers. */
_Static_assert(SYS_TOUCH_READ == 1 && SYS_TIME_US == 12, "device syscall IDs");
_Static_assert(SYS_TASK_ID == 16 && SYS_TASK_YIELD == 17 && SYS_TASK_EXIT == 18, "task syscall IDs");
_Static_assert(SYS_FILE_OPEN == 32 && SYS_FILE_REMOVE == 38, "file syscall IDs");
_Static_assert(sizeof(FileOpen) == 56 && offsetof(FileOpen, path) == 8, "file open ABI");
_Static_assert(sizeof(FileIo) == 272 && offsetof(FileIo, data) == 16, "file I/O ABI");
_Static_assert(sizeof(FileSeek) == 16 && sizeof(FileEntry) == 56, "file metadata ABI");
_Static_assert(SYS_FILE_OPEN_LONG == 40 && SYS_FILE_REMOVE_LONG == 41 && SYS_FILE_DIRECTORY_LONG == 42 &&
               sizeof(FileOpenLong) == 264 && sizeof(FilePathLong) == 256 && sizeof(FileDirectoryLong) == 528,
               "Long paths use additive syscall payloads");
_Static_assert(SYS_FILE_DIRECTORY == 39 && sizeof(FileDirectory) == 112 &&
               offsetof(FileDirectory, entry) == 64, "directory ABI");
_Static_assert(sizeof(TouchEvent) == 12 && sizeof(KeyEvent) == 8, "input ABI");
_Static_assert(sizeof(TextRow) == 116 && sizeof(CanvasButton) == 40, "graphics ABI");
_Static_assert(sizeof(MemoryStats) == 40 && sizeof(ConsoleInfo) == 64, "console info ABI");
_Static_assert(sizeof(ConsoleWrite) == 132, "console write ABI");
_Static_assert(SYS_NET_STATUS == 48 && SYS_NET_SCAN == 49 && SYS_NET_JOIN == 50 && SYS_NET_LEAVE == 51 &&
               SYS_NET_RESOLVE == 52 && SYS_NET_SOCKET == 53 && SYS_NET_TIME == 54 &&
               SYS_NET_PROFILE == 55 && SYS_NET_RANDOM == 56, "network syscall IDs");
_Static_assert(sizeof(NetStatus) == 136 && offsetof(NetStatus, ssid) == 16 && offsetof(NetStatus, address) == 60 &&
               offsetof(NetStatus, interface) == 88 && offsetof(NetStatus, rx_frames) == 112, "network status ABI");
_Static_assert(sizeof(NetScan) == 64 && offsetof(NetScan, ssid) == 24, "scan ABI");
_Static_assert(sizeof(NetJoin) == 112 && offsetof(NetJoin, passphrase) == 40 && offsetof(NetJoin, flags) == 104,
               "join ABI");
_Static_assert(sizeof(NetProfile) == 120 && offsetof(NetProfile, ssid) == 20 && offsetof(NetProfile, passphrase) == 52,
               "profile ABI");
_Static_assert(sizeof(NetTime) == 24 && offsetof(NetTime, unix_seconds) == 8, "time ABI");
_Static_assert(sizeof(NetRandom) == 264 && offsetof(NetRandom, data) == 8, "random ABI");
_Static_assert(sizeof(NetResolve) == 264 && offsetof(NetResolve, name) == 8, "resolve ABI");
_Static_assert(sizeof(NetSocketCall) == 1064 && offsetof(NetSocketCall, handle) == 8 &&
               offsetof(NetSocketCall, port) == 20 && offsetof(NetSocketCall, length) == 24 &&
               offsetof(NetSocketCall, state) == 32 && offsetof(NetSocketCall, data) == 40, "socket ABI");

int main(void) { return 0; }
