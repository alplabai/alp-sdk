### Fixed — GD32 bridge GPIO line 8 is unrouted and is now refused (#2724)

`meta-alp-sdk/recipes-kernel/linux/linux-renesas/0005-gpio-add-gd32-bridge-expander-driver.patch:302` ("GD32_UNROUTED_LINE") is refused with `-ENODEV` in `.request()` (`:991`), and `meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-som.dtsi:414` ("unrouted, gd32-bridge-firmware#298") no longer names line 8 `IO24`. A rejected write to that line used to persist in the sticky `output_mask` and break every replay.
