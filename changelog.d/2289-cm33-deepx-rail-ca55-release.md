### Added — `examples/v2n/v2n-cm33-deepx-rail` re-landed, with a fail-closed CM33-side CA55 core 0 release helper (#2289)

The CM33-boot DEEPX rail example from the closed PR #2300 is back on `dev`,
adapted to current `dev` (portable `<alp/*>` API for BRD_I2C and the two DEEPX
GPIOs, `da9292_ch2_sequence()` from #2483, the RIIC8-ownership exception kept:
its own board overlay is the only thing that enables `&i2c8` for a CM33 build,
and only until the CA55 is released).

Decision recorded in #2289: in CM33 cold boot (BOOTSELCPU low) the CM33 releases
CA55 core 0 after the rail sequence, with the reset vector pointed at a
CM33-staged BL2 through `SYS_ACPU_CFG_RVAL0/RVAH0`; BL2 keeps training DDR and
no OTP is involved. `v2n_cm33_release_ca55()` in the example's
`src/ca55_release.c` implements the vector write and the Table 2.2-7 cold-reset
release (`CPG_RST_1`, `CPG_RST_0`, `CPG_LP_CA55_CTL1/CTL2` handshakes), with the
offsets and bit fields taken from the #2289 comment that checked them against
RZ/V2N HW manual R01UH1071EJ0120 Rev.1.20, for example the vector register at
`examples/v2n/v2n-cm33-deepx-rail/src/ca55_release.c:44` ("#define SYS_ACPU_CFG_RVAL0").
The FSP headers are not in the tree, so a cross-check against them is marked
`TODO(alp-sdk#2289)`.

It is called only after `da9292_ch2_sequence()` returns `ALP_OK`, behind the
default-off `CONFIG_V2N_CM33_RELEASE_CA55`, and today always fails closed: the
AWO to ALL_ON power-domain entry (Table 4.5-4) has no register offsets in the
issue or the tree, so `all_on_entry()` at
`examples/v2n/v2n-cm33-deepx-rail/src/ca55_release.c:111` ("static alp_status_t all_on_entry")
returns `ALP_ERR_NOSUPPORT` and nothing is written. The `CPG_RSTMON_0` CA55 bit
mask is likewise missing and not guessed. Build-verified only; bench steps 1-6
of the issue are still open.
