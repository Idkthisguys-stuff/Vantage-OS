#include <vantage/kernel/usb_media.h>

static UsbFat32 volume;
static UsbFat32Directory directory;
static UsbFat32File entries[USB_FILE_LIST_LIMIT];
static uint32_t generation, revision, state, count, scanning;
static struct {
    uint64_t owner;
    uint32_t index, offset, length;
    UsbFat32Reader cursor;
    uint8_t data[USB_FILE_CHUNK];
} reader;

void usb_media_detach(void) {
    if (state == USB_MEDIA_ABSENT) return;
    volume = (UsbFat32){0};
    state = USB_MEDIA_ABSENT;
    count = scanning = 0;
    reader.owner = 0;
    revision++;
}

int usb_media_attach(const UsbFat32Source* source) {
    usb_media_detach();
    if (generation == ~0U) return HW_NO_SPACE;
    generation++;
    directory = (UsbFat32Directory){0};
    int result = usb_fat32_mount(&volume, source);
    state = result == HW_OK ? USB_MEDIA_READY :
            result == HW_UNSUPPORTED ? USB_MEDIA_UNSUPPORTED : USB_MEDIA_ERROR;
    scanning = result == HW_OK;
    revision++;
    return result;
}

void usb_media_poll(void) {
    if (!scanning) return;
    int result = usb_fat32_step(&volume, &directory, &entries[count]);
    if (result == USB_FAT32_MORE) return;
    if (result == HW_OK) count++;
    else if (result < 0) state = USB_MEDIA_ERROR;
    scanning = result == HW_OK && count < USB_FILE_LIST_LIMIT;
    revision++;
}

uint32_t usb_media_revision(void) { return revision; }

static int read_chunk(uint64_t owner, UsbFileCall* call, const UsbFat32File* file) {
    if (call->offset > file->bytes) return FILE_BAD;
    int same = reader.owner == owner && reader.index == call->index;
    if (!(same && reader.offset == call->offset)) {
        UsbFat32Reader cursor = {0};
        if (call->offset) {
            if (!same || call->offset != reader.cursor.offset) return FILE_BAD;
            cursor = reader.cursor;
        }
        uint8_t sector[USB_FILE_CHUNK];
        int length = usb_fat32_read(&volume, file, &cursor, sector);
        if (length < 0) return length == HW_PROTOCOL ? FILE_CORRUPT : FILE_IO_ERROR;
        reader.owner = owner;
        reader.index = call->index;
        reader.offset = call->offset;
        reader.length = (uint32_t)length;
        reader.cursor = cursor;
        memcpy(reader.data, sector, reader.length);
    }
    call->length = reader.length;
    memcpy(call->data, reader.data, reader.length);
    return 0;
}

int usb_media_call(uint64_t owner, UsbFileCall* call) {
    if (!owner || call->operation > USB_FILES_READ) return FILE_BAD;
    call->state = state;
    call->count = count;
    call->scanning = scanning;
    if (call->operation == USB_FILES_STATUS) { call->generation = generation; return 0; }
    if (state != USB_MEDIA_READY || call->generation != generation) return FILE_UNAVAILABLE;
    if (call->index >= count) return FILE_NOT_FOUND;
    const UsbFat32File* file = &entries[call->index];
    call->size = file->bytes;
    memset(call->name, 0, sizeof(call->name));
    memcpy(call->name, file->name, sizeof(file->name));
    call->length = 0;
    if (call->operation == USB_FILES_ENTRY) return 0;
    memset(call->data, 0, sizeof(call->data));
    return read_chunk(owner, call, file);
}
