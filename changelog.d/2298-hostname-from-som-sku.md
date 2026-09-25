### Changed — V2N-family boards take their hostname from the fitted SoM, not the image (#2298)

The hostname used to come from the image build (`alp.conf`'s static
`hostname ?= "alp-e1m"`, and `${MACHINE}` before that), so a board running
another SKU's image named itself after that SKU: an E1M-V2M103 booting a
`e1m-v2n101-a55` image reported `e1m-v2n101-a55`. U-Boot patch
`0009-rzv2n-dev-ALP-E1M-publish-sku-to-chosen.patch` now publishes the SKU
from the validated identity-EEPROM manifest to the kernel as
`/chosen/alp,sku`, and the new `alp-hostname` recipe (in every ALP image)
sets the hostname from it at boot, e.g. `e1m-v2m103`. Without a validated
manifest nothing is published and the distro default stays.
