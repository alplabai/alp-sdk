### Added — U-Boot can boot from the E1M-X EVK microSD: SDHI1 is now `mmc1` (#2282)

U-Boot's boot-medium test is `mmc dev 1`, used by the vendor `bootcmd_check`
and by the ALP `CONFIG_BOOTCOMMAND` from patch 0002. On the E1M-X EVK the
microSD slot is on SDHI1 (`mmc@15c10000`). `rzv2n-dev.dts` pointed `mmc1` at
SDHI2 (`mmc@15c20000`) and left SDHI1 disabled, so U-Boot never looked at the
SD card and always booted from the eMMC. The V2N provisioning flow needs the
SD path: the unit boots the release image from microSD, and Linux then does
every production write.

The new
`meta-alp-sdk/recipes-bsp/u-boot/u-boot/0008-rzv2n-dev-ALP-E1M-sdhi1-microsd.patch`
has five parts:

- **Device numbering.** SDHI1 becomes `mmc1` and SDHI2 moves to `mmc2`. The
  SDHI0 eMMC stays `mmc0`, so the environment device does not change.
- **Card power and IO voltage.** Under `CONFIG_ALP_E1M_SD1_MICROSD` (on by
  default in the patched `rzv2n-dev_defconfig`), `board_init()` drives PA2
  (`uSD1_V_SEL`) low for 3.3 V. It turns PA3 (`SD1_SD1PWEN`) off for 20 ms and
  then on, so a card that Linux left in 1.8 V UHS signalling starts again at
  3.3 V after a warm reboot.
- **Pad setup.** The SD1CLK, SD1CMD and SD1DAT0-3 pads get drive strength 2
  and slew rate 0, with input enabled on CMD and DAT. This matches the Linux
  `sdhi1_pins` group.
- **Limits.** The slot runs at 3.3 V only, because this U-Boot has no UHS
  support. There is no card detect: an empty slot fails `mmc dev 1` by timeout,
  and the boot falls back to the eMMC as before.
- **Data cards still boot the eMMC.** Once `mmc1` is the microSD, any inserted
  card passes `mmc dev 1`. The ALP boot command now takes the SD branch only
  when `ext4size mmc 1:2 boot/Image` also succeeds, so a card without a boot
  image falls back to the eMMC instead of stopping at the U-Boot prompt.

The patch applies with and without the x103-only patch 0003. It has not been
tested on hardware yet. If the GD32 controls the carrier SDIO mux, a blank
GD32 may leave the mux off the microSD socket (see #2282).
