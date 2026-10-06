### Added — V2N/V2M: Linux remoteproc attaches to the TF-A-started CM33 (opt-in), and the CM33 -> CA55 doorbell can move to SPI 385 (#2660)

Decision Q52: adopt Renesas' RZ/V2N Linux remoteproc and keep TF-A boot. BL2
still starts the CM33 at power-on; `rz_rproc` probes with the core already
running (state `detached`), `echo start > state` attaches without touching it,
and `stop` / reload become possible without a SoC reboot. **Phase 1, not
bench-verified** — see `docs/rzv2n-m33-secure-boot.md` "Lifecycle" for the
ordered bench plan.

- **Opt-in.** `ALP_V2N_REMOTEPROC = "1"` in the `linux-renesas` bbappend
  applies patches `0018`–`0021`, merges `remoteproc.cfg` and installs the
  `cm33_rproc` node (`e1m-v2n-remoteproc.dtsi`); default `"0"` leaves images
  unchanged. `0019`/`0020` are the Renesas RZ Multi-OS Package v4.2.0
  binding and driver, imported unmodified (SPDX GPL-2.0 / GPL-2.0 OR
  BSD-2-Clause, Renesas authorship kept); `0018` makes the CPG a syscon;
  `0021` adds `alp,rz-userspace-ipc` so the kernel never parses the CM33
  resource table, because the userspace OpenAMP master keeps the vrings. The
  driver already supports attach; no attach patch was needed.
- **Production is attach-only (Q53).** The default `cm33_rproc` node carries
  `alp,rz-attach-only` (new in `0021`): no start/load and a `stop` that always
  fails with `-EPERM`, so a Linux root process cannot stop or reload the CM33, whose SRAM stays secure. The opt-in
  dev flag `ALP_V2N_CM33_SRAM_NS = "1"` (default `"0"`, no change) applies the
  new TF-A patch `0002-rzv2n-optional-non-secure-access-to-CM33-SRAM.patch`
  (`ALP_CM33_SRAM_NS=1`, TZC-400 SRAM 0/1 region 0 admits non-secure masters;
  written from the public TF-A tree, not from the Renesas package), drops
  `alp,rz-attach-only`, and installs `/lib/firmware/m33_sm.elf` through the new
  `alp-cm33-firmware` recipe on `alp-image-edge`. With the flag on, a Linux
  root process can rewrite CM33 code memory: dev images only, and
  `alp-image-prod` now `bb.fatal`s if it is set. No provisioning ship-check
  yet (the bundle carries no record of the flag).
- **Window stays put.** The OpenAMP window remains A55 `0x4f700000` / CM33
  `0x9f700000` (9 MiB): Renesas' accepted range `0x40010000`–`0x43EFFFFF`
  lies in the secure 128 MB ahead of `memory@48000000` here, and the range
  only gates CA55-space resource-table addresses the kernel never parses.
- **Doorbell switch.** New `include/alp/protocol/v2n_mhu_doorbell.h` holds the
  MHU-B offsets for both halves. Default stays SWINT unit 12 / GIC_SPI 404
  (bench-proven, #697); `ALP_V2N_DOORBELL_SPI = "385"` selects Renesas'
  `rsp_ch8_ns` (RSP of NS slot 8: CM33 sets `0x50480114`, A55 clears
  `0x10480118`) on the kernel DT, libalp_sdk
  (`ALP_SDK_V2N_DOORBELL_RSP_CH8`) and the CM33
  (`CONFIG_ALP_V2N_DOORBELL_RSP_CH8`) together.
- **HIL spec** `tests/hil/v2m103-x-evk/v2m103-remoteproc-lifecycle.yaml` and
  `tests/scripts/test_v2n_doorbell_contract.py` (offsets and SPI numbers agree
  across header, DT and the bitbake switch).
