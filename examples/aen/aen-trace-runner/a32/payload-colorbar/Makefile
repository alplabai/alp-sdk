# Makefile -- colour-bar test payload for the A32 stub (CP-A2).
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
# current FB B at 0x02600000. Pair it with an HE built -DTR_FB_B_ADDR=0x02200000u.
LDFLAGS = -T ../common/payload.ld -nostdlib -nostartfiles -Wl,-Map=colorbar.map -Wl,--no-warn-rwx-segments

.PHONY: all clean
all: colorbar.bin
	python3 ../stub/mkpayload.py info colorbar.bin \
		--launch-jlink colorbar-launch.jlink --halt-jlink halt.jlink $(JLINK_OPTS)

colorbar.elf: payload_start.o colorbar.o ../common/payload.ld
	$(CC) $(CFLAGS) $(LDFLAGS) payload_start.o colorbar.o -o $@
colorbar.bin: colorbar.elf
	$(OBJCOPY) -O binary -j .image $< $@
payload_start.o: ../common/payload_start.S ../common/stub_abi.h
	$(CC) $(CFLAGS) -c $< -o $@
colorbar.o: colorbar.c ../common/stub_abi.h ../../src/ipc/tr_mbox.h
	$(CC) $(CFLAGS) -c $< -o $@
clean:
	rm -f *.o colorbar.elf colorbar.bin colorbar.map *.jlink
