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
