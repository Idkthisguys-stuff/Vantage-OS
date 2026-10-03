CROSS    = aarch64-linux-gnu-
CC       = $(CROSS)gcc
LD       = $(CROSS)ld
OBJCOPY  = $(CROSS)objcopy
CFLAGS   = -Wall -O3 -ffreestanding -nostdinc -nostdlib -nostartfiles -mcpu=cortex-a72 -Iinclude
CFILES   = $(wildcard src/*.c)
OFILES   = $(patsubst src/%.c, out/%.o, $(CFILES))

.PHONY: all clean

all: out/prod/kernel8.img

out:
	@mkdir -p out
	@mkdir -p out/prod

out/boot.o: src/boot.s | out
	$(CC) $(CFLAGS) -c $< -o $@
	
out/%.o: src/%.c | out
	$(CC) $(CFLAGS) -c $< -o $@

out/prod/kernel8.img: out/boot.o $(OFILES) src/linker.ld
	$(LD) -T src/linker.ld out/boot.o $(OFILES) -o out/kernel8.elf
	$(OBJCOPY) -O binary out/kernel8.elf $@

clean:
	rm -rf out