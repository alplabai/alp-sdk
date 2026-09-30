# Makefile -- A32 resident stub (A32_APP at 0x80020000, runs from 0x0240C000).
CROSS   ?= arm-none-eabi-
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
SIZE     = $(CROSS)size

# Probe flags + -ffp-contract=off (determinism ruling) + -mno-unaligned-access
# (the table is built with the MMU off, where data accesses are Device-typed
# and an unaligned access faults) + no memcpy/memset pattern calls (no libc).
CFLAGS  = -mcpu=cortex-a32 -marm -mfpu=neon-fp-armv8 -mfloat-abi=hard \
          -O2 -ffreestanding -nostdlib -nostartfiles -std=c11 -Wall -Wextra \
          -ffp-contract=off -mno-unaligned-access -fno-tree-loop-distribute-patterns \
          -I../common -I../../src/ipc
LDFLAGS = -T link.ld -nostdlib -nostartfiles -Wl,-Map=a32_stub.map -Wl,--no-warn-rwx-segments

OBJS = start.o stub.o crc32.o

.PHONY: all clean test
all: a32_stub.bin
	$(SIZE) a32_stub.elf

a32_stub.elf: $(OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@

a32_stub.bin: a32_stub.elf
	$(OBJCOPY) -O binary $< $@

start.o: start.S ../common/stub_abi.h
	$(CC) $(CFLAGS) -c $< -o $@
stub.o: stub.c ../common/stub_abi.h ../common/crc32.h ../../src/ipc/tr_mbox.h
	$(CC) $(CFLAGS) -c $< -o $@
crc32.o: ../common/crc32.c ../common/crc32.h
	$(CC) $(CFLAGS) -c $< -o $@

test:
	python3 mkpayload.py --selftest
	python3 decode.py --selftest

clean:
	rm -f *.o a32_stub.elf a32_stub.bin a32_stub.map
