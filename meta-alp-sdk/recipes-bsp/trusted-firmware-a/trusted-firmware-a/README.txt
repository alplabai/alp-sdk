The alp LPDDR4X DDR params are NOT in this public repo (Renesas-gen_tool-derived,
SoM-hardware-specific config). They are supplied at build time by the private
alp-sdk-internal/meta-alp-sdk overlay (rsync'd onto this public layer tree at
build time -- see that repo's conf/bblayers-overlay.md), which carries both
files in the matching recipes-bsp/trusted-firmware-a/trusted-firmware-a/ dir so
the bbappend's FILESEXTRAPATHS resolves them:

  ddr_param_def_lpddr4-alp.c        8 GB / D16S32 (rzv2n-family default)
  ddr_param_def_lpddr4-alp-d8s32.c  4 GB / D8S32 (E1M-V2N103/V2M103 memory tier)

A public-only checkout (no overlay applied) fails at do_fetch -- SRC_URI can't
resolve a file:// entry that isn't present in any layer's FILESEXTRAPATHS --
not in the bbappend's do_compile:prepend bbfatal, which only fires in the
narrower case where the file is present but unreadable/misnamed. The prebuilt
bl2/fip also live in alp-sdk-internal (production-flashed onto the SoM xSPI by
ALP).

alif-console-uart-build-knobs.patch (#1979) is UNRELATED to the DDR params
above -- it targets Alif's TF-A fork (alifsemi/trusted-firmware-a_alif), not
Renesas's, and is fully PUBLIC (no SoM-hardware-specific content, no private
overlay needed). It gives platform_def.h and devkit_e7_sp_min_setup.c
#ifndef-guarded build knobs for the console UART register base and pinmux, so
a carrier whose console isn't on the Alif DevKit's UART2 doesn't need to hand-
patch the vendor tree. Gated :e1m-aen801 in the bbappend, which is INERT today
-- see that bbappend's comment for why.
