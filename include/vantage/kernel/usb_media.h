#pragma once
#include <vantage/abi/usb_files.h>
#include <vantage/drivers/usb_fat32.h>

/* Core 0 owns the volume and borrows the source until detach. */
int usb_media_attach(const UsbFat32Source* source);
void usb_media_detach(void);
void usb_media_poll(void);
uint32_t usb_media_revision(void);
int usb_media_call(uint64_t owner, UsbFileCall* call);
