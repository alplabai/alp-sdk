### Fixed — E1M-AEN EVK Linux device tree no longer lets UART2 win the console over UART5 (#1979)

PR #2485 fixed the TF-A/BL32 half of #1979 only (the console UART base
+ pinmux hardcoded to the Alif DevKit's UART2 in
`trusted-firmware-a_%.bbappend`). This fragment adds the Linux-side
half the same issue describes: Alif's `devkit_ex_dct_defines.h`
hardcodes `UART2_STATUS "okay"`, so `serial@4901a000` (UART2)
enumerates as `ttyS0` and takes `console=ttyS0` regardless of what
`aliases { serial0 = ...; }` names — the 8250 driver assigns `ttySN`
by probe order, not by the DT alias. The E1M-AEN EVK (E1M-AEN801/
E1M-AEN701 share one carrier PCB) does not bring UART2 out; its real
console header is SoC UART5 (`serial@4901d000`, `P3_4`/`P3_5`,
alternate function 2).

New `meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-aen-evk-console.dtsi`
(`meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-aen-evk-console.dtsi:32`
("&uart2 {")) disables `&uart2`, enables `&uart5`, sets
`aliases { serial0 = &uart5; }`, and `chosen { stdout-path =
"serial0:115200n8"; }`. New
`meta-alp-sdk/recipes-kernel/linux/linux-alif_%.bbappend`
(`meta-alp-sdk/recipes-kernel/linux/linux-alif_%.bbappend:32`
("SRC_URI:append:e1m-aen801 = \" file://e1m-aen-evk-console.dtsi\"")) gates
the fragment on `:e1m-aen801`/`:e1m-aen701`, mirroring the TF-A
bbappend's per-carrier knobs.

**INERT TODAY, same reason as the TF-A half.** No Scarthgap
`meta-alif-ensemble` exists publicly (#1968), and even the one public
branch (`devkit-ex-b0`) is Scarthgap-incompatible (#1971), so neither
`MACHINE=e1m-aen801-a32` nor `e1m-aen701-a32` parses. There is no
`linux-alif` recipe in any layer on `bblayers.conf` for this bbappend
to attach to, so it never runs, and `KERNEL_DEVICETREE` in
`e1m-aen801-a32.conf`/`e1m-aen701-a32.conf` is itself still
`# TBD(alif-hw-config)` — no board dts exists yet to `#include` this
fragment from.

**Verification is static only, and incomplete.** `dtc` is not
installed in this environment
(`which dtc` exits non-zero), and no vendor devkit-e8/-e7 dts is
vendored in this tree, so the fragment could not be compiled against a
real base — a brace-balance check (`{` == `}`) is the only mechanical
check run. `&uart2`/`&uart5` are the conventional lowercase node
labels (matching `e1m-x-evk.dtsi`'s own `serial0 = &scif;` idiom for
the V2N side) but are NOT confirmed against a real vendor dtsi, which
does not exist publicly yet (#1968, #1971) — the fragment's own header
comment flags this and says a wrong label fails loudly at `dtc` time,
not silently. Confirm both labels once a Scarthgap `linux-alif` recipe
lands before relying on this fragment.
