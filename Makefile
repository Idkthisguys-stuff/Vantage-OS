CC = aarch64-none-elf-gcc
LD = aarch64-none-elf-ld
OBJCOPY = aarch64-none-elf-objcopy

BUILD_DIR = build

C_SOURCES = $(wildcard init/*.c) \
            $(wildcard kernel/*.c) \
            $(wildcard arch/armv8/*.c) \
            $(wildcard mm/*.c) \
            $(wildcard drivers/*/*.c) \
            $(wildcard user/lib/*.c) \
            $(wildcard user/env/*.c)

S_SOURCES = $(wildcard arch/armv8/*.s)

OBJECTS = $(patsubst %.c, $(BUILD_DIR)/%.o, $(C_SOURCES))
OBJECTS += $(patsubst %.s, $(BUILD_DIR)/%.o, $(S_SOURCES))

CFLAGS = -Wall -O2 -ffreestanding -nostdinc -Iinclude -Iuser/include

all: $(BUILD_DIR)/kernel8.img

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel8.img: $(OBJECTS) arch/armv8/linker.ld
	$(LD) -T arch/armv8/linker.ld $(OBJECTS) -o $(BUILD_DIR)/kernel8.elf
	$(OBJCOPY) -O binary $(BUILD_DIR)/kernel8.elf $@

clean:
	rm -rf $(BUILD_DIR)