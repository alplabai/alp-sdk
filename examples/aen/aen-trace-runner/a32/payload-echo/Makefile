# Makefile -- CP-A4 echo payload for the A32 stub (links src/ipc/tr_mbox.c).
CROSS   ?= arm-none-eabi-
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
CFLAGS  = -mcpu=cortex-a32 -marm -mfpu=neon-fp-armv8 -mfloat-abi=hard \
          -O2 -ffreestanding -nostdlib -nostartfiles -std=c11 -Wall -Wextra \
          -ffp-contract=off -fno-tree-loop-distribute-patterns \
          -I../common -I../../src/ipc \
          -DTR_FB_B_ADDR=0x02200000u
# ^ Pinned to the ORIGINAL FB B (lcd_fb, MHU0 window skipped): this payload
# runs on the stub table (0x026 WB, 0x027 unmapped), so it cannot draw the
# current FB B at 0x025EA000. Pair it with an HE built -DTR_FB_B_ADDR=0x02200000u.
LDFLAGS = -T ../common/payload.ld -nostdlib -nostartfiles -Wl,-Map=echo.map -Wl,--no-warn-rwx-segments
OBJS    = payload_start.o echo.o tr_mbox.o libc_min.o

.PHONY: all clean
all: echo.bin
	python3 ../stub/mkpayload.py info echo.bin \
		--launch-jlink echo-launch.jlink --halt-jlink halt.jlink $(JLINK_OPTS)

echo.elf: $(OBJS) ../common/payload.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -lgcc -o $@
echo.bin: echo.elf
	$(OBJCOPY) -O binary -j .image $< $@
payload_start.o: ../common/payload_start.S ../common/stub_abi.h
	$(CC) $(CFLAGS) -c $< -o $@
echo.o: echo.c ../common/stub_abi.h ../../src/ipc/tr_mbox.h
	$(CC) $(CFLAGS) -c $< -o $@
tr_mbox.o: ../../src/ipc/tr_mbox.c ../../src/ipc/tr_mbox.h
	$(CC) $(CFLAGS) -c $< -o $@
libc_min.o: ../common/libc_min.c
	$(CC) $(CFLAGS) -c $< -o $@
clean:
	rm -f *.o echo.elf echo.bin echo.map *.jlink
