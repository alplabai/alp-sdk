### Removed — E1M-NX9101 / NXP i.MX 93 support (never produced)

The E1M-NX9101 module was never produced, so the SDK no longer carries it: the
SoM preset and `imx93` family metadata, the `nxp:imx9:imx93` SoC spec, the
`rpmsg-imx93` example, `vendors/nxp-imx93/`, the `e1m-nx9101-a55` Yocto machine,
the Zephyr board overlay, the HIL board directory, and the four Murata Wi-Fi
combo manifests that only listed it. `E1M-NX9xxx` no longer matches the SoM SKU
pattern.

Public symbols removed:

- `alp/chips/pca9451a.h` — every `pca9451a_*` function and type and every
  `PCA9451A_*` macro (driver `chips/pca9451a/`, `CONFIG_ALP_SDK_CHIP_PCA9451A`).
- `alp/ext/nxp/storage.h` — `alp_nxp_storage_otfad_provision`,
  `alp_nxp_storage_otfad_set_window`, `alp_nxp_storage_otfad_slot_t`,
  `ALP_NXP_STORAGE_OTFAD_WINDOW_COUNT`, `ALP_EXT_NXP_STORAGE_AVAILABLE`.
- `ALP_SOC_LCDIF_COUNT`, `ALP_CAP_HW_LCDIF`, `ALP_CAP_ID_HW_LCDIF` — no remaining
  SoC has an LCDIF; the `alp_cap_id_t` enumerators after it shift down by one.
- `ALP_CORE_M33` (`alp_core_id_t`) — the other enumerators keep their values.

Kconfig symbols removed: `ALP_SOC_NXP_IMX9_IMX93`, `ALP_SDK_CHIP_PCA9451A`,
`ALP_SDK_INFERENCE_BACKEND_ETHOS_U_N93`, `ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U65`,
`ALP_TFLM_ETHOS_U65`. The `excludedFamilies` key of `tier-a-library-ci.json` and
the `assert_exclusion_still_not_buildable` ratchet existed only for this SoM and
are gone with it. Every removal is recorded in `docs/abi/removed-symbols.json`.
