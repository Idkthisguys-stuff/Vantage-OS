#pragma once
#include <vantage/abi/files.h>

#define SYS_USB_FILES 43U
#define USB_FILE_CHUNK 512U
#define USB_FILE_LIST_LIMIT 256U

enum { USB_FILES_STATUS, USB_FILES_ENTRY, USB_FILES_READ };
enum { USB_MEDIA_ABSENT, USB_MEDIA_READY, USB_MEDIA_UNSUPPORTED, USB_MEDIA_ERROR };

/* STATUS returns the mounted generation and the catalog size. ENTRY and READ
 * require that generation and a zero-based index. Start a sequential read at
 * offset zero, then advance by length. The volume tracks one active read.
 * Repeating its last chunk is safe. */
typedef struct {
    uint32_t operation, generation, index, offset;
    uint32_t state, count, scanning, size, length;
    char name[16];
    uint8_t data[USB_FILE_CHUNK];
} UsbFileCall;
