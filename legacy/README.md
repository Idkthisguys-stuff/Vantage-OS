# Historical examples

This directory retains earlier filesystem, RAM-disk and runtime experiments.
They are excluded from the production source manifest and supported host suites.
Their original source attribution is retained.

- `fs/block.c` and `tests/ramdisk.c` are the independent historical RAM disk.
- `runtime/std.c` duplicates memory routines now provided by `lib/memory.c`.

The earlier FAT tutorial example came from the
[bztsrc Raspberry Pi tutorial](https://github.com/bztsrc/raspi3-tutorial/tree/master/0C_directory).
The current kernel's removable FAT32 reader is in `drivers/storage/usb_fat32.c`.

Do not add these files back to the kernel build as a shortcut. Integrating an
example requires reviewing its memory, API and license contracts and adding
tests without competing production symbols.
