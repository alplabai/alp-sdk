### Fixed — SD CLK pad input buffer was off, likely the real reason `SW_RST_CMD` never cleared (#2035)

Both SD overlays (`aen-sdcard-readout`, `aen-evk-demo`) omitted `input-enable`
on the SD CLK pad, on the reasoning that a push-pull SoC output needs no
read-enable. That reasoning describes the SoC pin, not this controller's
receive path: the DWC_mshc command/data state machines clock off CLK **looped
back from the pad**, so with the pad's input buffer off that loopback is dead
while every asynchronous signal (`PSTATE`'s CMD/DAT level bits,
`INTERNAL_CLK_STABLE`) keeps reading correctly — which is exactly why it went
unnoticed. It matches the observed signature: `SW_RST_ALL` completes, the
transmit clock comes up, `PSTATE` reads right, but `SW_RST_CMD` never
self-clears while `SW_RST_DAT` does (DAT's circuit was already idle). Alif's
DFP sets `PADCTRL_READ_ENABLE` on SD CLK in all four of its SD examples
(`demo_sd.c`, `demo_sdio.c`, `demo_sd_fatfs.c`, `demo_mci_mdk_cmsisrtos2.c`)
and the vendor Linux dts applies the same read-enable/Schmitt/8mA pad config
to CLK as to CMD/DATA. Same class of defect as the rotary-encoder
`PADCTRL_READ_ENABLE` trap earlier this session. Both overlay comments, which
stated the opposite conclusion, are rewritten so this cannot be quietly
reverted.

### Added — `aen-sdcard-readout` prints SD register state before and during the disk attempt (#2035)

New diagnostics, so the next bench load is informative either way this time:

* **Before `disk_access_init`:** the P14_1/P14_0 pinmux registers
  (`0x1A6031C4`/`0x1A6031C0`, pad config bits[23:16], read-enable bit 16 —
  proves the CLK fix above reached silicon, not just the devicetree),
  `CLKCTL_PER_MST` (`0x4903F00C`, bit 16 = SD peripheral clock enable), and
  the SDHC capabilities register raw (`0x48102040`).
* **During, before any `SW_RST` write:** every prior `NORMAL_INT_STAT`/
  `PSTATE` reading on this bench was taken *after* the driver's own timeout
  path had already written `SW_RST_CMD|SW_RST_DAT`
  (`sdhc_dwc_wait_cmd_complete()`), which clears command-complete — so an
  "INT STATUS = 0" reading taken that way carries no information at all. A
  low-priority background thread (`sd_diag_thread_fn`, `K_THREAD_DEFINE`,
  priority below `main()` so it only runs while `main()` is blocked inside
  the driver's own `k_event_wait()`) watches `PSTATE`'s `CMD_INHIBIT` bit for
  its 0→1 edge — the externally-observable side effect of the driver's own
  command-register write, since this app has no hook into the vendored
  driver's internal path — then samples `NORMAL_INT_STAT` + `PSTATE` 10 ms
  later, comfortably inside the driver's own ≥1000 ms command-complete
  timeout. Covers up to the app's first four commands (CMD0, CMD8, ACMD41,
  CMD2/3).

### Changed — both SD overlays cap the bus at 25 MHz and refuse 1.8V signaling (#2035)

Once CMD0 succeeds, the SD subsystem would otherwise attempt CMD11 and try a
1.8V I/O signaling switch through the 74LVC157 SDIO mux, which runs at 3.3V
and was never designed to pass a 1.8V logic level. `no-1-8-v;` is added to
both overlays' `sdhc0` node — this driver actually reads it
(`sdhc_dwc.c:972`: `props->host_caps.vol_180_support = config->no_1_8_v ? 0 :
...`), so it is a real behavioural change, not a documentation-only one.
`max-bus-freq` drops from `<50000000>` to `<25000000>`, this binding's
equivalent of vendor Linux's `max-frequency`. Vendor Linux additionally sets
`no-hispeed`; recorded rather than applied — `zephyr/drivers/sdhc/sdhc_dwc.c`
reads `high_spd_support` straight off the hardware `DWC_SDHC_CAP1_HIGH_SPEED_Msk`
capability bit with no devicetree override, so there is nothing to set for it
on this binding without a driver change, which is out of scope here.

### Fixed — `sdhc_dwc.c` read the SD base clock at exactly 2x its real value (#2035)

`DWC_SDHC_BASE_CLK_FREQ_Msk` is built from `DWC_SDHC_FREQ_SEL_Pos` (bit 8:
`0xFFU << 8`), but `sdhc_dwc_clock_set()` shifted the masked value right by
`DWC_SDHC_BASE_CLK_FREQ_Pos` (7) — one bit short of where the mask actually
puts the field. Shifting by one less than a field's own bit position always
doubles its value (the field's top bit becomes an extra low bit instead of
being shifted out), independent of the field's actual contents — confirmed
against every tested value with a standalone bit-math check, not by
inspection alone. `base_clk_mhz`, and therefore every `Clock set: N Hz` log
line this driver prints, was a computed number at 2x the real base clock, not
a measured one. Fixed the shift to use `DWC_SDHC_FREQ_SEL_Pos`, and removed
the now-fully-unused, misleadingly-named `DWC_SDHC_BASE_CLK_FREQ_Pos` constant
so the bug cannot be silently reintroduced. Not build-verified against this
worktree's own twister run: the worktree's West module resolution compiles
`/home/caner/alp-sdk/zephyr/drivers/sdhc/sdhc_dwc.c` (the main checkout), not
this worktree's own copy, for reasons unrelated to this change — read
`0x48102040` on hardware (now printed by the diagnostics above) to confirm the
real base clock once this fix reaches a build that actually compiles it.
