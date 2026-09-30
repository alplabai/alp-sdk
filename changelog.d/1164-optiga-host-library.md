### Added

- OPTIGA Trust M now runs Infineon's host library (`vendors/optiga-trust-m`, release-v5.8.3, MIT) through a PAL on the portable `alp_i2c_*` / `alp_uptime_ms` surface, so the same driver serves the A55 and MCU cores. `optiga_trust_m_read_product_info()` returns the 27-byte Coprocessor UID (object `0xE0C2`) and `optiga_trust_m_send_apdu()` runs a raw APDU session; both returned `ALP_ERR_NOSUPPORT` before. Bench, E1M-V2M103 2026W38-0001 (`/dev/i2c-8`, `0x30`): probe, UID read (firmware `80101071`, build `2564`) and a raw OpenApplication + GetDataObject pass 14/14 runs. Read-only: no key or data-object writes; Shielded Connection stays off until binding-secret provisioning is designed.

### Changed

- `optiga_trust_m_product_info_t` now has the real Coprocessor UID layout (`cim_id`, `platform_id`, `model_id`, `rom_mask_id`, `chip_type`, `batch_num`, `x_coord`, `y_coord`, `fw_id[4]`, `esw_build`), replacing a placeholder shape that did not match the chip.

### Fixed

- `optiga_trust_m_init()` could never find a fitted Trust M on silicon: it read `I2C_STATE` with a repeated-start write-read, which the part NACKs every time. It now writes the register address, stops, waits the part's guard time and reads, as upstream's physical layer does, and allows the library's own wake budget (200 tries at 1 ms, `PL_POLLING_MAX_CNT`) instead of 10. The `fake_optiga` emulator now NACKs a repeated start, so the chips suite catches the old form.
