CROSS    = aarch64-linux-gnu-
CC       = $(CROSS)gcc
LD       = $(CROSS)ld
OBJCOPY  = $(CROSS)objcopy

CFLAGS   = -Wall -O3 -ffreestanding -nostdinc -nostdlib -nostartfiles -mcpu=cortex-a72 -Iincludes -Iinclude
# Recursively search src/ and all its subdirectories for .c and .s files
CFILES   = $(shell find src -type f -name '*.c')
SFILES   = $(shell find src -type f -name '*.s' -o -name '*.S')

OBJECTS  = $(patsubst src/%.c, out/%.o, $(CFILES))
OBJECTS += $(patsubst src/%.s, out/%.o, $(patsubst src/%.S, out/%.o, $(SFILES)))

.PHONY: all clean

all: out/prod/kernel8.img

out/%.o: src/%.s
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

out/%.o: src/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
out/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
out/prod/kernel8.img: $(OBJECTS) src/linker.ld
	@mkdir -p out/prod
	$(LD) -T src/linker.ld $(OBJECTS) -o out/kernel8.elf
	$(OBJCOPY) -O binary out/kernel8.elf $@

clean:
	rm -rf out