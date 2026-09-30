# Makefile -- Cortex-A32 bare-metal memory probe.
# See docs/2026-09-22-a32-probe-spec.md for the boot contract this build
# targets (XIP from MRAM at 0x80020000, no libc, no startfiles).

CROSS   ?= arm-none-eabi-
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
SIZE     = $(CROSS)size
OBJDUMP  = $(CROSS)objdump

CFLAGS  = -mcpu=cortex-a32 -marm -mfpu=neon-fp-armv8 -mfloat-abi=hard \
          -O2 -ffreestanding -nostdlib -nostartfiles -std=c11 -Wall -Wextra
LDFLAGS = -T link.ld -nostdlib -nostartfiles -Wl,-Map=a32_probe.map

OBJS = start.o main.o

.PHONY: all size clean vectors

all: a32_probe.bin size

a32_probe.elf: $(OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@

a32_probe.bin: a32_probe.elf
	$(OBJCOPY) -O binary $< $@

start.o: start.S
	$(CC) $(CFLAGS) -c $< -o $@

main.o: main.c results.h
	$(CC) $(CFLAGS) -c $< -o $@

size: a32_probe.elf
	$(SIZE) a32_probe.elf

# First 16 instructions at the reset vector -- sanity-check target used
# during review, not part of the normal build.
vectors: a32_probe.elf
	$(OBJDUMP) -d --start-address=0x80020000 --stop-address=0x80020040 a32_probe.elf

clean:
	rm -f *.o a32_probe.elf a32_probe.bin a32_probe.map
