<!-- SPDX-License-Identifier: Apache-2.0 -->
# Bench steps: per-project Linux core ownership (#2660)

NOT RUN. The per-project Linux fragment and the DT-driven CM33 clock hold
(`0001-clk-renesas-rzv2h-cpg-cm33-owned-clocks.patch`) are covered by unit
tests only. Run these in order on an E1M-V2N101 / V2M101 on the X-EVK, after an
instance has `m33` in `candidates` and an `m33:` block in
`metadata/e1m_modules/v2n/core-ownership.yaml` (none does today). Use the
instance whose hardware is cleared (not `e1m_spi0`: `hw_blocked`, P90-P92 not
3.3 V tolerant; `e1m_uart0` waits for the P51 RX pull-up proof).

1. Baseline, SoM default: build the image with no `ALP_LINUX_OWNERSHIP_DTSI`,
   boot, confirm no `cm33-owned-clocks` error in `dmesg`, the GD32 SCI7 link is
   up ~15 s after power-on (no CM33 bus fault), and the instance's `/dev` node
   is the vendor-default state.
2. Add `ownership: {<instance>: m33}` to the project `board.yaml`.
3. Emit both sides from the one project:
   `python3 scripts/alp_project.py --input board.yaml --emit linux-ownership-dts --output build/generated/linux-ownership.dtsi`
   and the CM33 `--emit dts-overlay --core m33_sm` / `--emit zephyr-conf --core m33_sm`.
   Confirm the fragment has `&<label> { status = "disabled"; }` for the
   instance and its `cpg_clocks` in `renesas,cm33-owned-clocks`.
4. `python3 scripts/check_amp_pad_claims.py --project board.yaml` exits 0.
5. Set `ALP_LINUX_OWNERSHIP_DTSI` to the absolute path of that file in
   `local.conf`, rebuild the kernel (`do_configure` must print "using
   per-project ownership fragment" and the `--fragment` check must pass), flash.
6. Cold-cycle through the programmable PSU (not a warm reset). On the A55:
   the instance has no `/dev` node and is absent from `/proc/device-tree`
   status `okay`; `dmesg` logs each held clock from the CPG patch.
7. On the CM33 console: the instance's peripheral initialises and passes its
   own loopback/echo; no bus fault for 10 minutes (clock hold survives
   `clk_disable_unused` at ~15 s).
8. Reverse check: remove the override, regenerate, rebuild; the instance is
   back on Linux and the clock list is RSCI7-only.

Record results in the PR; do not mark the patch bench-validated before steps 6-7
pass.
