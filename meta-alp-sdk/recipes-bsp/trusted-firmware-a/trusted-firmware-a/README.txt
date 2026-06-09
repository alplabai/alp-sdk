The alp LPDDR4X DDR param (ddr_param_def_lpddr4-alp.c) is NOT in this public
repo (Renesas-gen_tool-derived, SoM-hardware-specific). It is supplied at build
time by the private alp-sdk-internal/meta-alp-sdk overlay layer (higher BBLAYERS
priority) via FILESEXTRAPATHS -- it lives in that layer's matching
recipes-bsp/trusted-firmware-a/trusted-firmware-a/ dir. A public-only build
fails fast in the bbappend's bbfatal. The prebuilt bl2/fip also live in
alp-sdk-internal (production-flashed onto the SoM xSPI by ALP).
