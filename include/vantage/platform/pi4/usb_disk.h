#pragma once
#include <vantage/drivers/pi4_usb.h>

int pi4_usb_disk_configure(unsigned int path, const uint8_t* config, size_t bytes);
void pi4_usb_disk_detach(void);
