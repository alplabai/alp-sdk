### Documented — V2N/V2M I2S is A55-only; the CM33 has no SSIU driver (#1171)

Linux plays through `rcar_sound` on SSIU1/SSIU2 (PR #2536). Zephyr and hal_renesas ship no RZ/V2N SSIU driver, so on `m33_sm` `zephyr_drv.c` is not built and `<alp/i2s.h>` binds the priority-0 wildcard `src/backends/i2s/sw_fallback.c` (`open`/`start`/`stop` succeed, `read`/`write` return NOSUPPORT). `docs/e1m-x-v2n-sdk-integration.md` records the manual sections, DMA path, Audio_CLKB-only clock rule and A55/CM33 ownership rules; `core-ownership.yaml` now attributes P44, P45, P46 and P47 to the A55.
