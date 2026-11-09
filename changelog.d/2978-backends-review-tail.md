### Fixed — storage and capability review tail (#2978, #2981)

**`zephyr_flash` rejects an `instance_id` above 255 with `ALP_ERR_INVAL` instead of wrapping it onto another flash area.** `flash_area_open()` takes a `uint8_t`, so `instance_id` 256 used to open area 0. A native_sim test covers it.

**`ALP_CAP_HW_CAN_FD`, `ALP_CAP_NEON`, `ALP_CAP_HELIUM_MVE` and the other flag caps now collapse to exactly 0 or 1, as `<alp/soc_caps.h>` documents.** They used to pass the raw `ALP_SOC_*` value through, which is `UINT16_MAX` when no SoC is selected. `scripts/gen_soc_caps.py` emits the change and the v0.18 ABI snapshot is refreshed; a unit test pins every flag cap on the unknown-SoC path.
