# Makefile -- A32 renderer image (flat .bin at 0x02500000, both cores) for the stub.
#   make         scene build (default): renderer.bin, renderer-launch.jlink
#   make golden  CP-A6 regression, golden DL + per-band CRC checks:
#                renderer-golden.bin, renderer-golden-launch.jlink
#   make prof    scene build + per-span-kind PMCCNTR counters (renderer.c):
#                renderer-prof.bin, renderer-prof-launch.jlink
# Both also write halt.jlink. Links src/render/{r3d_raster,r3d_math,r3d_scene,r3d_rig,proj,
# sprite,cam_pip}.c, src/ipc/{tr_mbox,tr_cam_view,tr_pslot}.c (cam_pip/tr_cam_view/tr_pslot:
# fix round 7 item 5, the live camera picture-in-picture), a32/common/crc32.c, newlib memcpy/memset
# (full libc.a: nano's are size-optimised byte loops, too slow for the 46 KB
# z-band clear) and libm's lroundf.
CROSS   ?= arm-none-eabi-
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
SIZE     = $(CROSS)size
V       ?= scene
ifeq ($(V),golden)
NAME     = renderer-golden
VDEFS    = -DRENDER_DL_GOLDEN=1
else ifeq ($(V),prof)
NAME     = renderer-prof
VDEFS    = -DRENDER_DL_GOLDEN=0 -DTR_RASTER_PROF=1
else
NAME     = renderer
VDEFS    = -DRENDER_DL_GOLDEN=0
endif
O        = build-$(V)
# TR_PANEL_ROTATE=90|270: the Riverdi RVT121 build (src/render/panel_rot.h) --
# the bands are turned onto the landscape panel as they are copied out. It must
# equal the HE image's -DTR_PANEL_ROTATE. Its own build dir, so rotated and
# portrait objects never mix; the output is still $(NAME).bin.
ifneq ($(TR_PANEL_ROTATE),)
VDEFS   += -DTR_PANEL_ROTATE=$(TR_PANEL_ROTATE)
O        = build-$(V)-r$(TR_PANEL_ROTATE)
endif
# -ffp-contract=off on every renderer build (plan sec 5): no FMA drift vs host goldens.
# ROPT: the image's optimisation; RASTER_OPT: r3d_raster.c's (the span / row /
# setup kernels), -O3 -funroll-loops as measured on silicon (a32/payload-isa,
# 2026-09-23: row loop 137.6 -> 107.2 cyc/row, setup 1661 -> 1414 cyc/tri,
# Gouraud 9 px span 270 -> 255 cyc vs -O2). Not the whole image: at -O3
# -funroll-loops everywhere __bss_end lands 1.9 KB under the DL (0x02580000).
# tests/host/runner.sh builds the A32 qemu r3d tests' r3d_raster.c at the same
# RASTER_OPT. A/B: make ROPT=... RASTER_OPT=...; docs/superpowers/specs/
# 2026-09-23-a32-isa-opportunities.md.
ROPT       ?= -O2
RASTER_OPT ?= -O3 -funroll-loops
CFLAGS  = -mcpu=cortex-a32 -marm -mfpu=neon-fp-armv8 -mfloat-abi=hard \
          $(ROPT) -ffreestanding -std=c11 -Wall -Wextra -Wdouble-promotion -Werror \
          -ffp-contract=off -ffunction-sections -fdata-sections -DRENDER_A32=1 $(VDEFS) $(RENDER_DEFS) \
          -I../common -I../../src/ipc -I../../src/render
# --gc-sections: the golden build links the scene objects but never calls
# them -- their zone textures (P15) must not count against its budget.
LDFLAGS = -T renderer.ld -nostdlib -nostartfiles -Wl,--gc-sections -Wl,-Map=$(O)/$(NAME).map -Wl,--no-warn-rwx-segments
OBJS    = $(addprefix $(O)/,start.o renderer.o render.o r3d_raster.o r3d_math.o r3d_scene.o r3d_rig.o proj.o sprite.o \
          tr_mbox.o crc32.o cam_pip.o tr_cam_view.o tr_pslot.o)
HDRS    = $(wildcard ../common/*.h ../../src/ipc/*.h ../../src/render/*.h ../../src/game/*.h ../../src/vision/*.h *.h) \
          ../../tests/host/tr_golden_dl.h
vpath %.c . ../../src/render ../../src/ipc ../common

.PHONY: all golden prof clean
all: $(NAME).bin
	$(SIZE) -A $(O)/$(NAME).elf | grep -E '^\.(image|data|bss) '
	python3 ../stub/mkpayload.py info $(NAME).bin \
		--launch-jlink $(NAME)-launch.jlink --halt-jlink halt.jlink $(JLINK_OPTS)
golden:
	$(MAKE) V=golden
prof:
	$(MAKE) V=prof

$(O)/$(NAME).elf: $(OBJS) renderer.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -lm -lc -lgcc -o $@
$(NAME).bin: $(O)/$(NAME).elf
	$(OBJCOPY) -O binary -j .image -j .data $< $@
$(O)/start.o: start.S ../common/stub_abi.h | $(O)
	$(CC) $(CFLAGS) -c $< -o $@
$(O)/r3d_raster.o: r3d_raster.c $(HDRS) | $(O)
	$(CC) $(CFLAGS) $(RASTER_OPT) -c $< -o $@
$(O)/%.o: %.c $(HDRS) | $(O)
	$(CC) $(CFLAGS) -c $< -o $@
$(O):
	mkdir -p $@
clean:
	rm -rf build-scene build-golden build-prof build-*-r* renderer.bin renderer-golden.bin renderer-prof.bin *.jlink
