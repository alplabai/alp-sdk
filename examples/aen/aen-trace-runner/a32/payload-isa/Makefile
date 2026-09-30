# Makefile -- A32 ISA microbenchmark payload (isa_bench.h) for the A32 stub.
#   make            -> isa-O2.bin (+ isa-O2-launch.jlink, halt.jlink)
#   make OPT=3      -> isa-O3.bin: same source at -O3 (the -O2 vs -O3 A/B)
#   make OPT=3 UNROLL=1 -> isa-O3u.bin: -O3 -funroll-loops
#   make check      -> isa_bench.c on the host cc and under qemu-arm (NEON),
#                      every bit-exact check must pass
# Flags otherwise == a32/renderer/Makefile's, so the kernels compile as there.
CROSS   ?= arm-none-eabi-
QEMU_ARM ?= qemu-arm
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
OPT     ?= 2
UNROLL  ?= 0
ifeq ($(UNROLL),1)
NAME     = isa-O$(OPT)u
XFLAGS   = -funroll-loops -DISA_OPT=0x10$(OPT)
else
NAME     = isa-O$(OPT)
XFLAGS   = -DISA_OPT=$(OPT)
endif
O        = build-$(NAME)
ARCH     = -mcpu=cortex-a32 -marm -mfpu=neon-fp-armv8 -mfloat-abi=hard
WARN     = -std=c11 -Wall -Wextra -Wdouble-promotion -Werror -ffp-contract=off
CFLAGS   = $(ARCH) -O$(OPT) $(XFLAGS) $(WARN) -ffreestanding -ffunction-sections -fdata-sections \
           -DRENDER_A32=1 -DISA_PAYLOAD=1 -I../common -I../../src/ipc -I../../src/render
LDFLAGS  = -T isa.ld -nostdlib -nostartfiles -Wl,--gc-sections -Wl,-Map=$(O)/$(NAME).map -Wl,--no-warn-rwx-segments
SRCS     = isa_bench.c isa_bench.h ../../src/render/r3d_raster.c ../../src/render/r3d.h ../../src/render/span.h \
           ../../tests/host/tr_golden_dl.h
TMP     ?= /tmp/tr-isa

.PHONY: all check clean
all: $(NAME).bin
	python3 ../stub/mkpayload.py info $(NAME).bin \
		--launch-jlink $(NAME)-launch.jlink --halt-jlink halt.jlink $(JLINK_OPTS)

$(O)/$(NAME).elf: $(O)/payload_start.o $(O)/vectors.o $(O)/isa.o $(O)/isa_bench.o $(O)/crc32.o isa.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(filter %.o,$^) -lc -lgcc -o $@
$(NAME).bin: $(O)/$(NAME).elf
	$(OBJCOPY) -O binary -j .image -j .data $< $@
$(O)/payload_start.o: ../common/payload_start.S ../common/stub_abi.h | $(O)
	$(CC) $(CFLAGS) -c $< -o $@
$(O)/vectors.o: vectors.S ../common/stub_abi.h | $(O)
	$(CC) $(CFLAGS) -c $< -o $@
$(O)/isa.o: isa.c isa_bench.h ../common/stub_abi.h ../../src/ipc/tr_mbox.h | $(O)
	$(CC) $(CFLAGS) -c $< -o $@
$(O)/isa_bench.o: $(SRCS) | $(O)
	$(CC) $(CFLAGS) -c $< -o $@
$(O)/crc32.o: ../common/crc32.c | $(O)
	$(CC) $(CFLAGS) -c $< -o $@
$(O):
	mkdir -p $@

check:
	cc $(WARN) -O2 -DISA_HOST_MAIN -o $(TMP)-host isa_bench.c ../common/crc32.c && $(TMP)-host
	$(CC) $(ARCH) $(WARN) -O$(OPT) $(XFLAGS) -DISA_HOST_MAIN --specs=rdimon.specs -o $(TMP)-qemu.elf \
		isa_bench.c ../common/crc32.c && $(QEMU_ARM) -cpu max $(TMP)-qemu.elf
	python3 decode_isa.py --selftest

clean:
	rm -rf build-isa-* isa-*.bin *.jlink
