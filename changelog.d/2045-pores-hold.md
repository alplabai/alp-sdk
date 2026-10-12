### Fixed — U-Boot holds the V2N-M1 DX-M1 in reset through the DEEPX rail ramp (#2045)

`alp_deepx_rail_bringup()` (U-Boot patch `0004`) now drives `M1_RESET`
(`PA6`, the DX-M1 `PORES_N`) low on entry, before any I2C access and
before `P64` (`DEEPX_CORE_0P75_EN`) enables the DX-M1 I/O, LPDDR5X and
PLL rails. Previously `PA6` was only driven on an abort, so on the normal
path `PORES_N` was left undriven while those rails ramped; with the
`PORES_N` pull-up planned for the next SoM revision it would follow
`M1_VDD_1V8` and release the DX-M1 before `VDDQ` was up.

The settle after `P64` goes high grows from 5 ms to 15 ms
(`ALP_DEEPX_PORES_HOLD_MS`), so `P64` to `PA6` release is now at least
16 ms including patch `0001`'s 1 ms mux settle. This matches the DEEPX
DX-M1 reference design, which holds `PORES_N` low with a BD5215G reset
supervisor for 11 ms after `VDD18IO` is valid.

On a V2M image whose on-module EEPROM manifest does not say `v2n-m1`
(an unprovisioned module), a successful rail step now releases `PA6`
through `alp_deepx_reset_release()` without touching the PCIe mux, which
stays gated on the manifest. Before this change nothing drove `PA6` on
such modules and the DX-M1 left reset only through its internal
`PORES_N` pull-up; with `PA6` now held low, it would otherwise never
leave reset.

On an abort, `alp_deepx_rail_safe_off()` now asserts `PA6` before
dropping `P64`, so the DX-M1 is in reset before its rails go down.

Checked by re-applying the full U-Boot patch chain (`0004` to `0017`)
on the bitbake work tree and cross-compiling `rzv2n-dev.c` with
`-Werror`. Not yet run on silicon.
