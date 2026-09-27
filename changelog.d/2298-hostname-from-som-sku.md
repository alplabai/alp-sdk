### Changed — V2N-family boards take their hostname from the fitted SoM, not the image (#2298)

The hostname came from the image build: current images all report
`alp-e1m` (`alp.conf`'s `hostname ?= "alp-e1m"`), and an older image built
for `e1m-v2n101-a55` running on an E1M-V2M103 reported `e1m-v2n101-a55`.
U-Boot patch `0009-rzv2n-dev-ALP-E1M-publish-sku-to-chosen.patch` now
publishes the SKU from the validated identity-EEPROM manifest to the
kernel as `/chosen/alp,sku`, and the new `alp-hostname` recipe (in every
ALP image) sets the hostname from it at boot, e.g. `e1m-v2m103`. The
change needs the updated U-Boot (FIP) flashed; with an older bootloader,
or without a validated manifest, nothing is published and `alp-e1m` stays.
