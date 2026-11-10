#pragma once
#include <vantage/base.h>

#define FILE_PATH_MAX 256U
#define FILE_LEGACY_PATH_MAX 48U
#define FILE_IO_MAX 256U
#define FILE_LIMIT 256U
#define FILE_HANDLE_LIMIT 16U
#define FILE_CAPACITY 1048576U

#define FILE_READ 1U
#define FILE_WRITE 2U
#define FILE_CREATE 4U
#define FILE_TRUNCATE 8U

enum FileError {
    FILE_BAD = -1, FILE_NOT_FOUND = -2, FILE_NO_SPACE = -3,
    FILE_ACCESS = -4, FILE_BAD_HANDLE = -5, FILE_BUSY = -6,
    FILE_UNAVAILABLE = -7, FILE_CORRUPT = -8, FILE_IO_ERROR = -9
};

#define SYS_FILE_OPEN 32U
#define SYS_FILE_READ 33U
#define SYS_FILE_WRITE 34U
#define SYS_FILE_SEEK 35U
#define SYS_FILE_CLOSE 36U
#define SYS_FILE_LIST 37U
#define SYS_FILE_REMOVE 38U
#define SYS_FILE_DIRECTORY 39U
#define SYS_FILE_OPEN_LONG 40U
#define SYS_FILE_REMOVE_LONG 41U
#define SYS_FILE_DIRECTORY_LONG 42U
#define FILE_REGULAR 1U
#define FILE_DIRECTORY 2U
/* Original 32..39 payloads retain 48-byte paths; 40..42 add long paths. Relative
 * kernel paths start at /ram; cwd is application state. cursor is 0..FILE_LIMIT.
 * STAT returns canonical entry/type/size; LIST returns direct children. */
enum { DIRECTORY_STAT, DIRECTORY_LIST, DIRECTORY_CREATE, DIRECTORY_REMOVE };
typedef struct {
    uint32_t operation, cursor, type, size;
    char path[FILE_LEGACY_PATH_MAX], entry[FILE_LEGACY_PATH_MAX];
} FileDirectory;
typedef struct {
    uint32_t operation, cursor, type, size;
    char path[FILE_PATH_MAX], entry[FILE_PATH_MAX];
} FileDirectoryLong;

typedef struct {
    uint32_t flags, reserved;
    char path[FILE_LEGACY_PATH_MAX];
} FileOpen;
typedef struct {
    uint32_t flags, reserved;
    char path[FILE_PATH_MAX];
} FileOpenLong;

typedef struct {
    uint64_t handle;
    uint32_t length, reserved;
    unsigned char data[FILE_IO_MAX];
} FileIo;

typedef struct {
    uint64_t handle;
    uint32_t offset, reserved;
} FileSeek;

typedef struct { char path[FILE_LEGACY_PATH_MAX]; } FilePath;
typedef struct { char path[FILE_PATH_MAX]; } FilePathLong;
typedef struct {
    uint32_t cursor, size;
    char path[FILE_LEGACY_PATH_MAX];
} FileEntry;
