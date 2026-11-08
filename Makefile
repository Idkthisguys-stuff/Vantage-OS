CC = aarch64-linux-gnu-gcc
LD = aarch64-linux-gnu-ld
OBJCOPY = aarch64-linux-gnu-objcopy

BUILD_DIR = out

C_SOURCES = $(wildcard init/*.c) \
            $(wildcard kernel/*.c) \
            $(wildcard kernel/sched/*.c) \
            $(wildcard kernel/sched/delays/*.c) \
            $(wildcard arch/armv8/*.c) \
            $(wildcard mm/*.c) \
            $(wildcard drivers/*/*.c) \
			$(wildcard drivers/*.c) \
            $(wildcard user/lib/*.c) \
            $(wildcard user/env/*.c)

S_SOURCES = $(wildcard arch/armv8/*.s)

OBJECTS = $(patsubst %.c, $(BUILD_DIR)/%.o, $(C_SOURCES))
OBJECTS += $(patsubst %.s, $(BUILD_DIR)/%.o, $(S_SOURCES))
MM_OBJECTS = $(patsubst %.c, $(BUILD_DIR)/%.o, $(wildcard mm/*.c))

CFLAGS = -Wall -O2 -ffreestanding -nostdinc -Iinclude -Iuser/include
# Firmware discovery runs with the MMU off: unaligned Device accesses can fault.
CFLAGS += -mstrict-align -MMD -MP
$(MM_OBJECTS): CFLAGS += -Wextra -Werror
$(BUILD_DIR)/kernel/memory.o: CFLAGS += -fno-tree-loop-distribute-patterns
$(patsubst %.c, $(BUILD_DIR)/%.o, $(wildcard kernel/sched/*.c) arch/armv8/task_port.c): CFLAGS += -Wextra -Werror

all: $(BUILD_DIR)/prod/kernel8.img

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/prod/kernel8.img: $(OBJECTS) arch/armv8/linker.ld
	$(LD) -T arch/armv8/linker.ld $(OBJECTS) -o $(BUILD_DIR)/kernel8.elf
	@mkdir -p $(dir $@)
	$(OBJCOPY) -O binary $(BUILD_DIR)/kernel8.elf $@

clean:
	rm -rf $(BUILD_DIR)

-include $(OBJECTS:.o=.d)
