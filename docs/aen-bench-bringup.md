@page docs_aen_bench_bringup AEN (E8) bench bring-up + flashing

# E1M-AEN801 (Alif Ensemble E8) — bench bring-up, flashing, and validation results

Everything below was confirmed on **real E1M-AEN801 silicon** during the
2026-06-15 bench session (J-Link PLUS over SWD + the Alif SETOOLS over the
SE-UART). It is the authoritative companion to [`bring-up-aen.md`](bring-up-aen.md)
and [`aen-provisioning.md`](aen-provisioning.md).

## 1. What is validated on silicon

| Subsystem | Result | Notes |
|---|---|---|
| **SE / debug access** | ✅ M55-HE reachable | Generic `Cortex-M55` J-Link device works (CPUID `0x411FD220`, SW-DP IDR `0x4C013477`). **Update (J-Link V9.46, 2026-06-16): the AE822 part-number device profile (`AE822FA0E5597LS0_M55_HE`) also connects fine** — and it is *required* for the Flow D MRAM flash loader (the generic profile has none). An older J-Link DLL may fail to connect with the part-number device; if so, update J-Link or use the generic profile for read/RAM-run (Flows B/C). |
| **Production MRAM flash** | ✅ end-to-end | SETOOLS `app-gen-toc` + `app-write-mram` over the SE-UART; device auto-enters maintenance (no strap); SES loads + boots the ATOC (blink ran at `0x58000000`). |
| **Zephyr boot (alp-sdk image)** | ✅ first light | Boots to the idle thread; "Hello World" read back via RAM console over SWD. |
| **M55-HP core (second M55)** | ✅ first light (2026-06-17) | The HP core is held in reset at power-on (only the HE core's AP shows a CPUID); released by SES booting an **`M55_HP` ATOC** (`cpu_id=M55_HP`, `loadAddress=0x50000000` = HP ITCM global, vs HE's `0x58000000`). Proven alive by an advancing **SRAM0 liveness beacon** (`0x02000000`: magic `0xA11FE000` + CPUID `0x411FD220` + heartbeat that advances across a re-read) — read over the system/HE AP, not the HP AP. Example `examples/aen/aen-hp-core-smoke`; helper `scripts/bench/aen/flash-jlink-hp.sh`. Unblocks the HE↔HP MHUv2 doorbell. |
| **Dual-core deferred-TOC release** | ✅ bench-proven on E8, both directions (2026-07-31, 2026-08-01) | Releasing a peer M55 has two working recipes and they are **not interchangeable by direction**: plain `["load","boot"]` + `se_service_boot_cpu()` (service 501) works HP-master→HE-peer (proven 2026-06-17 and re-confirmed 2026-08-01) but a real Alif silicon defect makes it fail HE-master→HP-peer (`CFSR=0x00000101` IACCVIOL+IBUSERR, `PC=0xEFFFFFFE` — Alif's SE Host Services API docs, v1.109.0 p.115, name "the M55-HP core in Ensemble devices" as a case where "resetting the core also invalidates its TCM content"). `["load","boot","deferred"]` + `se_service_process_toc_entry()` (service 500, loads at runtime AFTER the reset instead of before it, per p.115's own reset→reload→release remedy; ships unpatched in hal_alif v2.3.0) works **both** directions — bench-proven HP-master→HE-peer (2026-08-01, 16/16 pongs, `uLs  D`→released) and is the only proven way to do HE-master→HP-peer. Portable surface: `CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC` in `src/backends/mproc/alif_se_boot.c`, default **ON when the peer is HP** (501 is vendor-documented broken there) and **OFF when the peer is HE** (501 proven fine there, twice). See § Flow A — Dual-core deferred-TOC boot below for the full asymmetry table and vendor citations. |
| **UTIMER counter** (Tier-1.5) | ✅ PASS *after a fix* | As-merged it never counted (read 0); fixed in **PR #158** (missing `alif_utimer_enable_soft_counter_ctrl`). Re-validated: counter advances. |
| **GPIO** (`gpio_dw`, Tier-1) | ✅ PASS *(re-proven 2026-07-27 on silicon; earlier same-day CKEN theory REFUTED — see below)* | DDR/DR set+readback correct via the Zephyr GPIO API (J-Link ground truth) — but that alone is only the gpio_dw controller-register path: the original PASS criterion also read `EXT_PORTA` and treated it as pad-level proof, and on this controller `EXT_PORTA` mirrors `SWPORTA_DR` for an OUTPUT-direction pin (Synopsys DW_apb_gpio databook), so it could never independently fail and proved nothing beyond DR/DDR — **this half still stands.** Earlier the same day, `CLKCTL_PER_SLV->GPIO_CTRL[n]` bit 16 (`GPIO_CTRL_CKEN`, the per-port GPIO functional-clock enable) — clear on every port and never written by alp-sdk — was suspected as the reason a pad looked electrically dark, and a fix was added (`zephyr/drivers/gpio/gpio_clk_alif.c`, PR-tracked). **That theory is REFUTED, decisively, on the same bench**: after a cold reset with CKEN still clear, driving `SWPORTA_DDR`/`SWPORTA_DR` from the debugger moved the pad — `0x49002050 = 0x00000010` with `0x4902F088 = 0x00000100` (bit 16 unset). CKEN is not required for pad drive. The real explanation for "the LED was dark": the old `blink` example toggled ~10 times over ~2 s and returned with the pad left LOW — a window nobody was watching, not a pad that couldn't move. Once `blink` was changed to loop forever, the maintainer confirmed **by eye that the LED blinks** — GPIO output on the E8 pad is now proven on real silicon, with REN enabled at `0x1A603050`, `EXT_PORTA` following `SWPORTA_DR` 12/12 on P2_4 while `blink` ran. **Not yet proven:** the colour is wrong (`EVK_PIN_LED_RED` lights GREEN) — still being measured, no colour conclusion asserted here — and until `gpio11`–`gpio14` landed (§ this doc, dtsi), the green/blue RGB channels (P12_7/P12_6) had no controller to reach at all. `gpio_clk_alif.c`'s CKEN write is kept (it matches Alif's own documented `enable_gpio_clk()` init) but is no longer claimed to fix a dark pad. See `examples/aen/aen-gpio-bench/src/main.c`. |
| **I2C2 + 24C128 EEPROM** (`i2c_dw`, Tier-1) | ✅ PASS | EEPROM ACKs at 0x50 and returns a **populated Alp manifest** (not blank) — magic `ALPH`, SKU, serial, mfg date, CRC-32 all decode; one of 12 devices on the bus — once the pinctrl carries the **pad config** Alif's reference uses — `input-enable` (REN) + `bias-pull-down` (DSC=2). See §3. |
| **PWM** (Tier-1.5) | ✅ PASS | pwm_set_cycles reg readback matches (CNTR_PTR/COMPARE/CTRL), shares the hal_alif UTIMER start-path the counter fix validated. |
| **SPI** (`alif,dwc-ssi-spi`, Tier-2) | ✅ PASS *after a fix* | DWC-SSI stayed in slave mode → `spi_transceive` -116 (TX FIFO full, no SCLK). The Alif SoC gates master mode behind `CLKCTRL_PER_SLV.SSI_CTRL` (`0x4902F028`), which upstream never sets. **PR #162** sets it in the driver. Re-validated: `rc=0`, internal-loopback `rx==tx`, CTRLR0=`0x80002007`. See §3. |
| **Ethernet** (`alif,ethernet` / `eth_dwmac`, Tier-1.5) | ✅ PASS *after a fix* | Real cause of the long no-link: the GMAC DMA descriptor rings + net_buf pool sat in the M55 **DTCM** (`zephyr,sram = &dtcm`), which is **not** on the GMAC DMA bus. Fix: `zephyr,sram = &sram0` (global on-chip SRAM `0x02000000`, CPU addr == DMA addr) + `CONFIG_DCACHE=n`. The PHY power (`E_PHY_PWRDWN` = P15_4), reset (`E_PHY_RESET` = P11_6), and RCSR bit7 `REF_CLK_SEL=1` were already correct. Re-validated end-to-end: DHCP lease `192.168.10.137` (server-side dnsmasq lease + ARP `REACHABLE`). See §3. |
| **UART3** (`ns16550`, Tier-1) | ✅ PASS | Internal loopback. |
| **Counter** (`utimer0`, Tier-1.5) | ✅ PASS | UTIMER0 counter advances. |
| **Counter alarm** (`utimer0` COMPARE-A, Tier-1.5) | ✅ PASS *after a fix* (RAM-run, 2026-06-17) | The COMPARE-A one-shot **alarm** fires + re-arms (`fired=2`). Two bring-up bugs fixed in `counter_alif_utimer.c`: (1) the match interrupt compares the `COMPARE_A_BUF1` **shadow** register (`0xD4`), not the `COMPARE_A` reg (`0xD0`) the driver wrote — so the shadow stayed 0 and bit2 only matched at the start `CNTR==0` tick; (2) the alarm's NVIC line is `comp_a_buf1` (the bit2 event), not `comp_capt_a` (bit0/CAPTURE_A) — so even once bit2 latched its line was never enabled. Regression: `examples/aen/aen-counter-alarm-regcheck`. |
| **WDT** (CMSDK, Tier-1) | ✅ PASS | CMSDK watchdog. |
| **ADC** (`adc_alif`, Tier-2) | ✅ PASS | Single-shot read. |
| **DAC** (`dac_alif`, Tier-2) | ✅ PASS | Write path holds (code-side; analog output bench-unverified). |
| **Camera stack** (`cam`/`csi`/`dphy`/`arx3a0`) | ✅ PASS *(bind)* | All four nodes BIND + the v4.4-ported drivers load; `cam` instantiation is DT-blocked and live capture is HW-blocked (no sensor wired). |
| **Ethos-U85** (NPU) | ✅ PASS | ID `0x20007001`. |
| **Ethos-U55-HE** (NPU) | ✅ PASS | ID `0x10104201`. |
| **NPU inference** (TFLM + Ethos-U85) | ✅ PASS | Tiny fixture runs to completion. Real models from MRAM slot0: **person_detect** (100% NPU) + **keyword_scrambled** (mixed 6-NPU/9-CPU, via the `<6>` op-resolver) both `runJob=OK` (2026-06-17). See `examples/aen/aen-npu-inference-person-mram`. |
| **PDM mics** | ✅ PASS | Live varying PCM = real audio. |
| **I2S TX** (`i2s3`) | ✅ PASS | Clocks the tone out with the 76.8 MHz audio clock (audible amp output pends the 74LVC157 mux + TAS2563 config). |
| **Quadrature encoder** (`qenc`) | 🟡 PARTIAL *(HW-gated)* | Driver reads clean; count is static until the encoder is physically spun. Not a code/Flow-D bug. |
| **SD card** (DWC SDHC) | 🟡 PARTIAL *(HW-gated)* | SDHC inits but the card is unreachable until the EVK SDIO 74LVC157 mux (EN=IO20 / SEL=IO21, both CC3501E-side) is routed and a card is inserted. Not a code/Flow-D bug. |
| **GPU2D** (`<alp/gpu2d.h>` sw_fallback) | ✅ PASS (RAM-run, 2026-06-17) | Portable 2D surface on the M55-HE via the priority-0 pure-C software fallback (the D/AVE 2D HW backend is opt-in + bench-unverified). `fill_rect` + clip, `blit`, and all four `blend` modes (REPLACE/SRC_OVER/ADDITIVE/MULTIPLY) produce **exact** expected pixels on silicon. Example: `examples/aen/aen-gpu2d-bench`. The D/AVE 2D hardware backend (`alif_dave2d.c`) is the separate bucket-C item. |
| **Low-power (WFI/SysTick)** | ✅ PASS (RAM-run, 2026-06-17) | Stage-A baseline: the M55-HE enters architectural `__WFI()` via the kernel idle path (a k_timer beats the wake cadence; SysTick wakes it) for N rounds — proven by an advancing SRAM0 beacon + uptime (8 sleeps, 0→420 ms). No `CONFIG_PM` (pinned Zephyr 4.4 ships no Alif PM); the deep IWIC `pm_state_set` path (WICCONTROL HE `0x1A604010`) is the documented Stage-B follow-on. Example: `examples/aen/aen-power-smoke`. |
| **SE CryptoCell compute** (`<alp/security.h>` SHA / AES-GCM) | ✅ PASS (RAM-run, 2026-06-19) | The portable hash/AEAD surface runs **inside the Secure Enclave's CryptoCell** on the E8: the `se_cryptocell` backend binds at priority 110 and pushes SHA-256 / AES-128-GCM into the SE over the RTSS-HE↔SE MHUv2 pair (the new public `se_service_send_request()` transport, hal_alif patch `0002`). Bench: SHA-256(`"abc"`) matched the NIST known-answer (`s=0`, MATCH) and the AES-128-GCM encrypt→decrypt round-trip matched (`enc=0 dec=0`, MATCH) — both computed in the SE, plus SE TRNG. `CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL_SEND_SEAM` now defaults ON; algs the SE declines fall through to MbedTLS-PSA. Example: `examples/aen/aen-se-crypto`. |
| **CRC engine** (`crc_alif` / `alif,crc`, Tier-1.5) | ✅ PASS (RAM-run, 2026-06-19) | HW CRC32-IEEE on `crc0@48107000` via the Zephyr CRC class API — `crc_begin/update/finish` rc=0, computed `0x684fc31c` = the reference value over a 16-byte input. Example: `examples/aen/aen-crc-regcheck`. |
| **HWSEM** (`hwsem_alif` / `alif,hwsem`, Tier-1.5) | ✅ PASS (RAM-run, 2026-06-19) | `hwsem@4902e000` take/give/count over the in-tree driver: count `0→1→0` across `take_busy`/`give` (master_id `0x410fd222`). Example: `examples/aen/aen-hwsem-regcheck`. |
| **LPTIMER** (`counter_alif_lptimer` / `alif,lptimer`, Tier-1.5) | ✅ PASS (RAM-run, 2026-06-19) | Always-on `lptimer@42001000` ch0 — 32768 Hz down-counter advances (3456 ticks / ~100 ms) via the portable `counter_*` API. Example: `examples/aen/aen-lptimer-regcheck`. |
| **Comparator (HSCMP)** (`comparator_alif` / `alif,cmp`, Tier-2) | ✅ PASS (RAM-run, 2026-06-19) | `cmp0@49023000` driven via the portable `comparator_*` API (output 1/1, internal DAC6 reference; the connect-but-don't-enable init held — no ISR storm). External pin/threshold edge-trigger = bench TBD (no analog stimulus). Example: `examples/aen/aen-cmp-regcheck`. |
| **Secure boot** (MCUboot ECDSA-P256 chain) | ✅ PASS (bench-proven at `0da1f1b4`) | SES → MCUboot (ITCM) → slot0 (MRAM XIP) → application boots with `CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256=y` + `CONFIG_BOOT_VALIDATE_SLOT0=y` (read back from the built `mcuboot/zephyr/.config`): `PC=80012FBC`, `VTOR=80010800`, `CFSR=00000000`, `IPSR=000`. Verification proven live, not inferred from a boot: flipping one byte of the TLV `0x22` signature (offset `0x4a30`, `0xda`→`0xdb`, TLV `0x10`/SHA-256 and TLV `0x01`/key intact) produces `D: bootutil_verify_sig: ECDSA builtin key 0` then `E: Unable to find bootable image` — the check runs to completion. `SIGNATURE_TYPE_NONE` + `VALIDATE_SLOT0=y` boots in twelve seconds (watched ten minutes, CycleCnt advancing). This verification run was `CONFIG_SINGLE_APPLICATION_SLOT=y`. Verified backend is TinyCrypt (`CONFIG_BOOT_ECDSA_TINYCRYPT=y`), not PSA; `.config` confirms `CONFIG_SINGLE_APPLICATION_SLOT=y` + `CONFIG_FLASH_BASE_ADDRESS=0x0`. **Separately, a swap-using-scratch build boots and logs** `I: Bootloader chainload address offset: 0x10000` — **boot only; the swap/rollback path itself was not exercised `[UNTESTED]`.** Still requires `CONFIG_DCACHE=n`, `ROM_START_OFFSET=0x800`, and the `zephyr/patches/mcuboot` `do_boot` patch. **Customer path proven too (second session):** a plain-J-Link `loadbin` of an imgtool-signed image to slot0 `0x80010000` — no SETOOLS/ATOC/SE-UART — is verified + chainloaded and survives repeated cold power-cycles; **proven at `0x80010000` only** (ATOC region / erasing MCUboot untested); both refusal shapes (tampered sig, non-MCUboot image) leave the debug port alive (`Secure debug: enabled`, halts + single-steps cleanly). Single-slot result (`CONFIG_SINGLE_APPLICATION_SLOT=y`) — A/B swap / OTA untested. See `docs/aen-provisioning.md` §0.5 and `docs/secure-boot.md`. |

The flow-D batch (17 aen-* apps) booted on real E8 at **15 PASS, 2 PARTIAL** (both
hardware-gated). A 2026-06-19 Flow-C RAM-run pass then **reconfirmed** the SE-crypto
offload + the CRC / HWSEM / LPTIMER / HSCMP driver bodies (rows above) and the LPRTC
counter (§ below); the quadrature encoder stays PARTIAL (live count needs the encoder
physically spun — not a code bug). **That 15/2 tally predates the 2026-07-27
GPIO re-classification above** — one of the 15 PASS entries was `gpio_dw`, whose
original pass criterion is now known to have been unable to fail (see the GPIO
row). GPIO has since been re-proven PASS by a different, decisive check (the
maintainer's optical confirmation that `blink` blinks, plus 12/12 `EXT_PORTA`
agreement while it ran) — the CKEN gate that this note used to say the tally
was waiting on turned out not to be the blocker (see the GPIO row); the 15/2
tally still has not been formally recomputed against the corrected GPIO
criterion, but nothing in the correction moves GPIO out of the PASS column.

## 2. The four flashing / observation flows

| Flow | Use it for | Touches MRAM? | Tooling |
|---|---|---|---|
| **A. Production MRAM flash (SETOOLS/ISP)** | shipping image, QA, re-keying | **Yes** | SETOOLS over the SE-UART (`west flash` = `alif_flash` runner) |
| **B. Console observation** | watching app output during bring-up | No | RAM console over SWD, or SEGGER RTT |
| **C. J-Link RAM-run** | dev/debug iteration without burning MRAM | No | J-Link `loadbin` to ITCM + `go` |
| **D. J-Link MRAM flash** | fast MRAM burn from the SWD probe (no SE-UART) | **Yes** | J-Link + the **Alif J-Link device support pack** (see below) |

A decides *what runs*; B decides *how you watch it*; C is the fast inner loop;
D is the faster A (burns MRAM straight from the SWD probe).
On this bench the only USB serial is the FT232R **SE-UART** (flow A), so the app
console is not on USB — which is why flow B exists.

> **Flows A and D both burn MRAM** — A drives the burn through the SE over the
> SE-UART (ISP), D drives it through the SWD probe. The earlier blanket claim
> "J-Link does not write MRAM on this part" was bench-specific, not a hardware
> limit: J-Link *can* burn Alif MRAM with the part-number device profile (the MRAM
> loader is built into J-Link V9.46+; no separate pack). **Both flow A and flow D
> are now working on this bench** (flow D enabled 2026-06-17 after a probe swap —
> see § Flow D). Flow D is the day-to-day default now: a burn is ~0.16 s over SWD
> with no SE-UART maintenance-window race / power-cycle dance.
>
> **Both flows also provision the same two app shapes.** Bench-proven 2026-07-19
> on real AE822 silicon: a single `app-write-mram -c <uart> -p` run over the
> SE-UART (flow A) burns **both** the ITCM-load embedded ATOC and the slot0-XIP
> two-blob variant (standalone app blob at `0x80010000` + the ATOC, two
> `COMMAND_BURN_MRAM` phases, byte-exact read-back) — the `alif_flash` west
> runner now auto-detects the shape from the app's own reset vector. Flow D's
> `scripts/bench/aen/flash-jlink-mramxip.sh` two-blob helper (§ Flow D below)
> is therefore a **speed / SE-UART-reset-race alternative**, not a capability
> requirement — SE-UART was never limited to ATOC-only.

> **Runnable helpers.** The shell helpers that drive all four flows below
> (build, Flow A `flash-run.sh`, Flow C `ram-run.sh`, Flow D `flash-jlink.sh` +
> the `flash-all-flowd.sh` batch, plus `reread.sh`) are checked into
> [`scripts/bench/aen/`](../scripts/bench/aen/) with host-specific values
> (`SETOOLS_DIR`, `SE_UART`, the J-Link probe) sanitized into a sourced
> `bench-env.sh`. See `scripts/bench/aen/README.md`
> for per-script usage and the env-var table. **The Alif SETOOLS are
> license-gated and are not redistributed by alp-sdk** — obtain them from Alif
> and `export SETOOLS_DIR=...` before running Flow A/D.

### Flow A — Production MRAM flash (SETOOLS, no strap/jumper)

```bash
cd <setools>/app-release-exec-linux
# 1. Build the ATOC (app-only config keeps the factory DEVICE config).
./app-gen-toc -f build/config/<cfg>.json
# 2. Write to MRAM over the SE-UART. The device AUTO-enters maintenance
#    (SET_MAINTENANCE_FLAG + reset) — no jumper, no strap. Baud 57600, dynamic.
#    <your-serial-device>: your OS's port name for the SE-UART adapter --
#    see docs/cross-platform-setup.md §7.7 for the per-OS naming convention.
./app-write-mram -c <your-serial-device> -p .
```

A clean write ends `100% ... Done`; on reset the SES loads + boots the ATOC
(M55-HE `loadAddress 0x58000000`). `west flash` on the carrier wraps this via the
**`alif_flash`** runner — it does **not** use J-Link, and auto-detects this
ITCM-load shape vs. the slot0-XIP shape (§ Flow D) from the app's own reset
vector, so both provision over the SE-UART with no flag. Pre-provisioned Alp
Lab modules ship a dev-signed MCUboot + self-test in slot0 (LCS=DM), so
`west flash` works day-1; the manual path above is only for re-keying or
recovering a bare module. A pre-provisioned module also takes a plain
J-Link `loadbin` straight to slot0 at `0x80010000` **only** (ATOC region
/ erasing MCUboot untested) with no SETOOLS/ATOC/SE-UART at all — see
the **Secure boot** row in §1 above and `docs/aen-provisioning.md`
§0.5 for the exact sequence.

### Flow A — Dual-core deferred-TOC boot

Booting a **second, dependent** M55 image (a peer the master releases at
runtime, not one that boots freestanding) has two working ATOC recipes, and
which one to use **depends on which core is the peer being released**, not
on which recipe is "correct":

| Direction | plain `["load","boot"]` + `se_service_boot_cpu()` (service 501) | `["load","boot","deferred"]` + `se_service_process_toc_entry()` (service 500) |
|---|---|---|
| HP master → HE peer | **works** (2026-06-17, re-confirmed 2026-08-01: `uLV`, Dest Addr `0x58000000`, 28.52 ms load, 16/16 pongs) | **works** (2026-08-01, in-tree `aen-rpc-pingpong`: `uLs D`→released, 16/16 pongs) |
| HE master → HP peer | **fails** — vectors from empty memory, lock up (see below) | **works** — only proven way to release an HP peer |

**This is a vendor-documented asymmetry, not a general defect in the plain
recipe.** Alif's SE Host Services API docs (`SE_Host_Services_API_v1.109.0.pdf`):

- p.112, `SERVICES_boot_cpu` (service 501): *"For the M55 cores, there are
  cases in which this service does not work. The currently known case is
  the **M55-HP core in FUSION REV_Bx devices**, where resetting the core
  also invalidates its TCM content."*
- p.112, `SERVICES_boot_cpu`: *"This service does not perform image
  loading, verification, etc., it just boots the core... You would need to
  use an ATOC to achieve these."*
- p.115, `SERVICES_boot_release_cpu`: *"in some cases, resetting the
  core also invalidates its TCM. A known case is the **M55-HP core in
  Ensemble devices**. Because of that, after calling
  `SERVICES_boot_reset_cpu()` to stop the core, the image in the TCM must
  be reloaded, before calling `SERVICES_boot_release_cpu()` to start the
  core."*
- p.112, `SERVICES_boot_process_toc_entry` (service 500): *"The TOC entry
  should also be in a DEFERRED state... This SERVICE call will un-defer the
  TOC entry. This is a higher-level function... a convenient way to boot a
  CPU core."*
- SETOOLS guide `AUGD0005` p.35: *"DEFERRED – The image will be skipped at
  boot time (i.e., no boot or load) and wait for a service request at
  runtime."*

The two vendor passages disagree with each other on scope (p.112 says
"FUSION REV_Bx devices", p.115 says "Ensemble devices" with no qualifier)
— quoted verbatim rather than resolved; E8 is an Ensemble part, so p.115
covers it either way. **The mechanism**: the ATOC's `["load","boot"]` entry
places HP's image in its TCM **at power-on**; `boot_cpu`'s release later
resets the core, and that reset invalidates the TCM the power-on load
already filled — load-then-reset is the failing order. p.115's own
documented remedy is **reset → reload → release**: reload strictly AFTER
the reset, not before it. This matches every bit measured on E8
(bare-Zephyr repro, 2026-07-31, HE master releasing an HP peer via 501):
the SES table reported the HP entry `uLV` (Loaded, Verified), but the
peer's ITCM read as uninitialized SRAM at every sample from t+0.80s to
t+60s, and releasing it produced `CFSR = 0x00000101` (IACCVIOL + IBUSERR),
`PC = 0xEFFFFFFE`. Not "Loaded is a lie" — the bytes were placed at
power-on and then wiped by the release-time reset. Releasing an **HE**
peer via 501 has no such defect documented or observed in either bring-up
session (2026-06-17, 2026-08-01).

The deferred recipe reorders load to AFTER the reset instead of avoiding
the reset: flag the peer entry `"flags": ["load", "boot", "deferred"]`.
`"deferred"` is a member of the `flags` **array** (a sibling `"deferred":
true` key is rejected: `ERROR: Invalid key: "deferred"`). It sets
`TOC_IMAGE_DEFERRED = 0x100` in the entry's flags word (`0x00000022` →
`0x00000122`) and prints `D` in the SES table — per AUGD0005 p.35, deferred
means **no boot-time load at all** (not "loaded but not released"): the SES
table shows `uLs  D`, Dest Addr blank, Time `0.00 ms`, because nothing was
placed in ITCM at power-on. The master image releases the peer at
**runtime** with `se_service_process_toc_entry()` (service 500), which
performs the load, verify AND release together, in the reset → reload →
release order p.115 requires — the load happens strictly after whatever
reset the release involves, so the image the core actually vectors from is
never the one a reset just wiped. With this recipe (HP master → HE peer) an
RPMsg link carried 495 consecutive PING/PONG round-trips over 4m11s in a
bare-Zephyr repro (2026-07-31), and 16/16 in the in-tree `aen-rpc-pingpong`
example (2026-08-01).

A **third recipe exists and is not used here**: Alif's own DevKit-e8
DualCore example
(`Boards/DevKit-e8/Examples/DualCore/.alif/M55_HP_HE_mram_cfg.json`) gives
both cores `mramAddress` + `flags: ["boot"]` only — no `"load"` at all, so
both execute XIP straight from MRAM and no ITCM/TCM copy (and therefore no
invalidation risk) is ever in play. alp-sdk's AEN examples link for ITCM by
default; adopting MRAM-XIP for both cores simultaneously would be a bigger
boot-model change than this backend's scope, but it's a real alternative
worth evaluating if the deferred-TOC path ever proves awkward.

`se_service_process_toc_entry()` ships **unpatched** in the west-pinned
hal_alif v2.3.0 module (`services_lib_ids.h SERVICE_BOOT_PROCESS_TOC_ENTRY=500`,
`se_services/zephyr/src/se_service.c`) — unlike `se_service_boot_cpu()`
(hal_alif patch `0001-se-service-add-boot-cpu.patch`), no hal_alif patch is
needed for this path. This is a property of that pinned module revision,
not a general hal_alif guarantee — a separate `alif-dfp-ref` tree used for
vendor-doc research does not carry either service-500 or service-501
wrapper at all.

The portable surface is
[`CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC`](../zephyr/kconfigs/mproc-rpc-usb.kconfig)
in `src/backends/mproc/alif_se_boot.c`: when ON, `alp_mproc_boot_core()`
(`<alp/mproc.h>`) calls `se_service_process_toc_entry()` with the ATOC entry
id from `CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC_ENTRY_ID` instead of
`se_service_boot_cpu()`. A companion
`CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC_PEER_IS_HP` names which core
this build's entry id refers to; `alif_se_boot_core()` rejects any other
core with `ALP_ERR_NOSUPPORT` (the documented meaning of "a core the
platform boots by other means", `<alp/mproc.h>`) instead of silently
un-deferring the configured entry regardless of which core was actually
asked for. This guard is deferred-path-only: with `PEER_IS_HP` set but
`DEFERRED_TOC` off, `boot_core()` still takes the plain 501 path for
whichever core is asked, unguarded. **Default: ON when
the peer is HP** (501 is vendor-documented broken there, and no working
legacy HP-peer deployment exists to preserve), **OFF when the peer is HE**
(501 is bench-proven fine there twice; flipping the default would demand
every existing HE-peer ATOC be reflashed with the `"deferred"` flag or this
path fails against it).

Bench tooling: `scripts/bench/aen/flash-run-dualcore.sh <hp-build-dir>
<he-build-dir>` emits and flashes the two-entry ATOC over the SE-UART (Flow
A) — `ALP-HP` normal (`["load","boot"]`), `ALP-HE` deferred
(`["load","boot","deferred"]`), i.e. the HP-master shape. It generalizes
`flash-run.sh`'s single M55-HE entry; unlike `flash-run.sh` it does not
auto-read a RAM console — prove both cores independently (HP via
`reread.sh`, HE via your app's own IPC/beacon proof). An HE-master ATOC
swaps which entry carries `"deferred"` and is not yet scripted.

`flash-update-log-dual.sh` (`examples/connectivity/firmware-update-log`,
Flow D) emits its HE-client entry as plain `"flags": ["load"]` with an
HP-owner master — that is the HP-master→HE-peer direction, which the table
above shows works fine with the plain recipe; it is not the broken
pattern.

### Flow B — Seeing the console

| Carrier | HE console UART | Pins |
|---|---|---|
| Alif Ensemble DevKit | UART2 | P1_0 / P1_1 |
| E1M carrier (E1M-EVK) | UART5 | P3_4 / P3_5 |

The `examples/aen/*` apps now **default to the Alp UART console** on this UART
(the board `_defconfig` selects it, and each app's `prj.conf` sets
`CONFIG_SERIAL=y` / `CONFIG_UART_CONSOLE=y` / `CONFIG_UART_INTERRUPT_DRIVEN=y`
explicitly): attach a 115200 8N1 terminal to the carrier console UART and
`printk()`/`RESULT` lines appear directly. A few apps deliberately keep the RAM
console (the `dualcore-*` pair, `hp-core-smoke`, `mcuboot-smoke`, `power-*`, and
`uart-ns16550-loopback` — each documents why in its `prj.conf`).

On a bench whose only USB serial is the SE-UART (no terminal on the app console),
fall back to a **RAM console** read over SWD — each converted `prj.conf` ships
this as a commented block: uncomment `CONFIG_RAM_CONSOLE=y`,
`CONFIG_RAM_CONSOLE_BUFFER_SIZE=2048`, `CONFIG_UART_CONSOLE=n` (and comment the
four UART lines). `printk()` then accumulates in `ram_console_buf`; resolve its
address (`nm zephyr.elf | grep ram_console_buf`) and `mem8 <addr>, 0x800` in
J-Link, then ASCII-decode. Have each test print one `RESULT PASS: ...` /
`RESULT FAIL: ...` line. SEGGER **RTT** is the live-terminal alternative over the
same SWD link.

### Flow C — J-Link RAM-run (no MRAM write)

The SoC `select`s XIP, so retarget the ROM region to ITCM — **as a bench-only
overlay applied via `-DEXTRA_DTC_OVERLAY_FILE`, never by editing the app's own
overlay** (the retarget is a bench concern; none of the six `aen-*` bench apps
carries it in-tree). The shipped overlay,
[`scripts/bench/aen/aen-flowc-itcm.overlay`](../scripts/bench/aen/aen-flowc-itcm.overlay),
carries exactly this DTS — **use the path-reference form** (`<&itcm>` makes
`FLASH_SIZE=0` → link overflow):

```dts
/ {
    chosen {
        zephyr,flash = &itcm;          /* NOT <&itcm> */
        /delete-property/ zephyr,code-partition;
    };
};
```

`prj.conf` must set `CONFIG_ALP_SDK=y` (the Tier-1.5 UTIMER Kconfig is gated under
it) + the flow-B RAM console. Build with both module paths
(`-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>`), then:

```
JLinkExe -device Cortex-M55 -if SWD -speed 4000 -nogui 1   # GENERIC device
J-Link> halt
J-Link> loadbin build/zephyr/zephyr.bin <base>   # loadbin's implicit reset re-reads
J-Link> halt                                     # our freshly-loaded ITCM vectors:
J-Link> go                                       # core is already at our reset handler
```

`<base>` is the app's link base, **not always `0x0`**: the overlay above only
retargets the ROM *region* to ITCM, it does not reset a `prj.conf`'s own
`CONFIG_FLASH_LOAD_OFFSET`. The retarget is really two independent, BOTH-required
settings, each a committed artifact under `scripts/bench/aen/`:

> **Since alp-sdk#1067 the ITCM retarget is required for EVERY Flow C RAM-run,
> not just for the apps that hard-code the offset.** The AEN board `_defconfig`
> now sets `CONFIG_USE_DT_CODE_PARTITION=y`, so a plain `west build` links into
> MRAM slot0 (`FLASH_LOAD_OFFSET=0x10000`, reset vector `0x8001xxxx`) — which is
> what Flow A/D and `alif_flash` need, and what Flow C must undo. `ram-run.sh`
> refuses a slot0-linked image (exit 5) rather than mis-running it. An app whose
> own `boards/*.overlay` already carries the two `chosen` lines below needs no
> extra fragments; one that deletes `zephyr,code-partition` from `&itcm` instead
> of from `/chosen` does **not** — that form is a no-op and now mislinks.

- the devicetree half — [`aen-flowc-itcm.overlay`](../scripts/bench/aen/aen-flowc-itcm.overlay)
  (`zephyr,flash = &itcm;`, path-ref not `<&itcm>`, +
  `/delete-property/ zephyr,code-partition;`), which stops Zephyr from deriving
  the link offset from a DT code-partition, applied via
  `-DEXTRA_DTC_OVERLAY_FILE`, **and**
- the Kconfig half — a **Flow-C-only** conf fragment
  (`scripts/bench/aen/aen-flowc-itcm.conf`) that sets
  `CONFIG_USE_DT_CODE_PARTITION=n` **and** `CONFIG_FLASH_LOAD_OFFSET=0x0`, layered
  on top of the generic `scripts/bench/aen/aen-bench-shared.conf` (RAM-console
  observability + `CONFIG_DCACHE=n`, no link-offset override — that fragment is
  also used unmodified by Flow A/D, where overriding the link offset would be
  wrong), applied via `-DEXTRA_CONF_FILE`.

The conf half alone still links into MRAM (`CONFIG_FLASH_BASE_ADDRESS` stays
`0x80000000`) — it only stops Zephyr *deriving* the offset from the DT
code-partition; the overlay half is what moves the code-partition itself.
Pass both together:
```
scripts/bench/aen/build.sh <app-dir> \
    -DEXTRA_CONF_FILE="scripts/bench/aen/aen-bench-shared.conf;scripts/bench/aen/aen-flowc-itcm.conf" \
    -DEXTRA_DTC_OVERLAY_FILE="scripts/bench/aen/aen-flowc-itcm.overlay"
```

Both `aen-flowc-itcm.conf` lines are needed because they undo two different
things. `USE_DT_CODE_PARTITION=n` alone undoes the board `_defconfig`'s
*derived* offset, but it does **not** touch a hard-coded literal
`CONFIG_FLASH_LOAD_OFFSET=0x10000`. Seven examples hard-code that offset —
redundant since #1067 (Kconfig resolves the same `0x10000` with no warning) but
harmless, and they are still the reason the second line exists:
the five `aen-cc3501e-*` apps (`aen-cc3501e-ble-gatt`, `aen-cc3501e-bringup`,
`aen-cc3501e-companion-tour`, `aen-cc3501e-gatt-register`, `aen-cc3501e-gpio`)
and `aen-eeprom-manifest`, all via their own `prj.conf`, plus
`examples/peripheral-io/alp-console`, which sets it via a **BOARD-scoped**
conf instead —
`boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.conf`
(see `examples/peripheral-io/alp-console/README.md`). For any of these, a
Flow C RAM-run must ALSO carry the explicit `CONFIG_FLASH_LOAD_OFFSET=0x0`
override, since a later `EXTRA_CONF_FILE` fragment wins over both an app's
`prj.conf` and its board-scoped conf. Confirm the real link base from
the build (`readelf -l build/*/zephyr/zephyr.elf` — the LOAD segment with the
LOWEST `PhysAddr` among segments with a NONZERO `FileSiz`, **not** just the
first LOAD segment: an ITCM-retargeted link's first LOAD segment is often a
zero-FileSiz `.bss` segment in DTCM, and loading there corrupts live RAM), and
pass that as `<base>` — `scripts/bench/aen/ram-run.sh` derives it this way
automatically instead of assuming `0x0` or trusting segment order, refuses to
run an ELF whose derived base is `>= 0x80000000` (slot0/MRAM-linked — Flow C
cannot RAM-run that; rebuild with this retarget or use Flow D), and separately
refuses (exit 6) any derived base that isn't `0x0`, the ITCM global alias
(`0x50000000`/`0x58000000`), or SRAM (`0x02xxxxxx`) — a DTCM address slipping
through is exactly the failure mode this whole section warns about.

> **Reset caveat:** a J-Link reset asserts **SYSRESETREQ**, which reboots the
> **SES** (not just the M55). Prefer `loadbin`/`go`; don't `reset` mid-loop.
> **FIXME:** this contradicts the SYSRESETREQ scope claimed below (§Flow D,
> "only resets the M55, not the SE") — unresolved, needs a silicon check.

### Flow D — J-Link MRAM flash (built-in Alif loader, no SE-UART)

Burns MRAM directly from the SWD probe — **no SE-UART round-trip** — using the MRAM
flash loader that is **built into J-Link V9.46+** itself (no extra pack to install).
It is the fast equivalent of flow A and writes the **exact same two blobs to the exact
same addresses** SETOOLS burns, so the SE re-verifies + boots the image identically (no
re-signing, no keys).

**The one thing that unlocks it: the `AE822FA0E5597LS0_M55_HE` device profile.** The
loader only activates for the part-number device; with the generic `-device Cortex-M55`
(what read/RAM-run use) there is no loader and `loadbin` to MRAM does nothing useful.
That is the whole reason older notes said "J-Link doesn't write MRAM" — true for the
generic profile, false for the part-number one.

**The two blobs** (read the addresses from `app-write-mram`'s own `Burning:` line — they
shift per build/config, don't hardcode):

| Blob | Address | What it is |
|---|---|---|
| `zephyr_<app>_he.bin` | `0x80010000` | the app, linked at its slot0 `mramAddress` (`CONFIG_FLASH_LOAD_OFFSET=0x10000`) |
| `AppTocPackage.bin` | e.g. `0x8057f5b0` | the signed ATOC the SE reads at boot (near the top of the partition) |

> **Flow D needs the MRAM-XIP build, not the ITCM (flow C) build.** The app must link at
> `0x80010000` — sanity-check `xxd -l 8 zephyr.bin`: word 1 (reset vector) must read
> `…80012xxx`. `…80002xxx` means it linked at `0x80000000` and will fault on boot.

```bash
# 1. build (MRAM/slot0 link — board default chosen, NOT the flow-C &itcm overlay) + ATOC
ninja -C <build>                                   # -> zephyr/zephyr.bin
cp <build>/zephyr/zephyr.bin <setools>/build/images/zephyr_<app>_he.bin
./app-gen-toc -f build/config/app-zephyr-he.json   # -> build/AppTocPackage.bin
./app-write-mram -c <your-serial-device> -p .      # run once, read the "Burning:" addrs, Ctrl-C
#   <your-serial-device>: see docs/cross-platform-setup.md §7.7 for the per-OS port name
```

```
# 2. program from the Alif J-Link (JLinkExe on Linux / JLink.exe on Windows; same script)
si SWD
speed 4000
device AE822FA0E5597LS0_M55_HE          # NOT generic Cortex-M55 — this unlocks the loader
connect
loadbin <app>.bin   0x80010000
loadbin <apptoc>.bin 0x8057f5b0         # use the address from the "Burning:" line
verifybin <app>.bin  0x80010000
verifybin <apptoc>.bin 0x8057f5b0
mem32 0x80010000 2                       # reset vector must read 80012xxx
RSetType 2                               # nRESET PIN — mandatory (see below)
r
g
exit
```
Invoke: `JLinkExe -CommanderScript <script>` (Linux) / `JLink.exe -CommandFile <script>`
(Windows). On success J-Link prints `Program & Verify` + `Verify successful.` for both blobs and
`mem32 0x80010000 = … 80012…`. The post-`r` "connect under reset / Attach to CPU failed"
is **normal** — the pin reset re-booted the SE and the app is now running, so J-Link
can't re-halt the secure core. Read back a witness with a second `mem32` connect (memory
reads work while the CPU runs; register reads error out harmlessly).

**Why `RSetType 2` (nRESET pin) is mandatory.** It re-runs the **SE boot ROM**
(full-chip), which is what loads/verifies/boots the app from MRAM. A plain J-Link reset
(`AIRCR.SYSRESETREQ`) only resets the M55, not the SE, so the new image wouldn't boot.
**FIXME:** this contradicts the reset caveat above (Flow C, "reboots the SES, not just
the M55") — unresolved, needs a silicon check.
The pin reset also re-enumerates the FT232R SE-UART (shared reset domain) — which is why
you can't hold `app-write-mram` open across a reset, and why flow D sidesteps the SE-UART
ISP-window race entirely.

**Notes / limits:** J-Link can write **MRAM** but **not peripheral/secure registers**
(those writes are firewalled). Writing only the app (without the matching ATOC) fails
secure-boot verification — always write both consistent blobs.

> **Verified state on *this* Linux bench (alplab-gw, 2026-06-17): flow D WORKS.** The
> original probe (J-Link PLUS, old firmware) could not connect with
> `-device AE822FA0E5597LS0_M55_HE` — it only worked with the generic `-device
> Cortex-M55` (reads/RAM-run). Swapping to a probe on **J-Link V13 firmware (May 2026,
> matched to DLL V9.50)** fixed it: the AE822 profile now connects
> (`Connecting to J-Link ...O.K.` → `Found Cortex-M55 r1p0`), the built-in MRAM loader
> programs + verifies the ATOC over SWD (`Verify successful.`, ~0.16 s @ ~200 KB/s), and
> `RSetType 2` (nRESET pin) re-runs the SE boot ROM so the app boots from MRAM. Bench
> proof: flashed `aen-ethernet-link` over flow D → `RESULT PASS` + DHCP lease
> `192.168.10.137` (server-side dnsmasq lease + ARP `REACHABLE` confirm).
>
> **Two gotchas that block the probe (both bit us):** (1) a version-mismatched probe
> triggers a mandatory **J-Link firmware update on first connect**, and that update
> **times out over a USB hub** (`Communication timed out - Can not execute firmware
> update`) leaving the probe stuck (`Cannot connect to the probe/programmer`, enumerates
> but no session) — plug the probe into a **direct root USB port**, not a hub, so the FW
> update completes. (2) The post-burn `r`/`g` prints "core did not halt / reconnecting" —
> that is **normal** (the pin reset reboots the SE, the app is running, J-Link can't
> re-halt the secure core); read a witness back over the generic device.
>
> Helper: `scripts/bench/aen/flash-jlink.sh <build-dir> [read-bytes]` runs this whole
> flow (gen-toc → AE822 connect → loadbin/verify the package at its per-build start
> address from `app-package-map.txt` → `RSetType 2`/`r`/`g` → RAM-console read-back). It
> writes the **single self-contained `AppTocPackage.bin`** (our ITCM-load-via-ATOC apps),
> not the slot0-XIP two-blob variant above. See
> `scripts/bench/aen/README.md` for all four flows.
>
> **Two-blob (slot0-XIP) helper — validated 2026-06-17.** For an app linked into MRAM
> slot0 (a real NPU model that overflows ITCM), `scripts/bench/aen/flash-jlink-mramxip.sh`
> runs the two-blob flow (app → `0x80010000` + the signed ATOC → its parsed address). Two
> facts the bench pinned down: the app entry's `mramAddress` is the **full** address
> `0x80010000` (the `0x10000` *offset* gives SETOOLS `Invalid Global Address`), and the
> image needs **`CONFIG_USE_DT_CODE_PARTITION=y`** so it links at the slot0 offset
> (`0x8001xxxx` reset vector) instead of the MRAM base (`0x8000xxxx`, which faults) —
> since #1067 the board `_defconfig` supplies that, so no app sets it. Proven
> by `examples/aen/aen-npu-inference-person-mram` (the real `person_detect` MobileNet run
> from MRAM → `RESULT PASS`). The same shape provisions over the SE-UART with plain
> `west flash` / `app-write-mram` too (§ above) — this script is the faster SWD-only path,
> not the only path.

## 3. Board HW requirements found on the bench

- **I2C2 pads need the right pinctrl config (NOT external pull-ups).** The
  on-module 24C128 EEPROM is on **SoC I2C2** (P5_6 `SCL_C` / P5_7 `SDA_C`,
  bridge/DNP-selected) and works on the internal pulls. The original example
  pinctrl set only `pinmux`, so the controller couldn't sense SDA → it NACKed
  every address (looked like a dead/no-device bus). The fix matches Alif's own
  reference i2c pinctrl (`sdk-alif .../ensemble-pinctrl.dtsi`): add
  **`input-enable`** (sets the pad REN bit so `i2c_dw` can read SDA/SCL — ACK
  detect + clock-stretch) and **`bias-pull-down`** (upstream `pinctrl_soc.h`
  encodes this as the pad driver-state-control field **DSC=2**, exactly Alif's
  I2C value — `bias-pull-up` gives DSC=1 and a dead bus; the upstream binding's
  pull naming is effectively inverted vs the Alif pad HW). With that,
  `examples/aen/aen-eeprom-manifest` reads the EEPROM at 0x50 — which ACKs and
  returns a **populated Alp manifest** (not blank): magic `ALPH`, SKU, serial,
  mfg date, and a matching CRC-32 all decode (the EEPROM is one of 12 devices on
  the bus). External pull-ups are only needed for fast-mode (400 kHz); 100 kHz
  works on the internal pulls.
- **UTIMER tick rate ≈ 400 MHz, not the 100 MHz placeholder.** The counter
  advanced ~800 k ticks per 2 ms busy-wait → real input ≈ 400 MHz, 4× the
  `clock-frequency = <100000000>` placeholder on the `utimer*` SoC nodes. The
  count is correct; only the µs↔ticks scale is off. Set the verified value from
  the Alif TRM (do not invent it).
- **Housekeeping trio (RTC/TMP/OPTIGA) routing, CORRECTED (#1848).** This entry
  originally read "is on the slave-only LPI2C0 this rev → Tier-3
  (SE-mediated); a next-rev respin moves it to a master-capable I2C (LPI2C0 →
  I2C0 on P7_0/P7_1)." That was wrong: it is already on I2C0 (function C,
  P7_0/P7_1) on the E1M-AEN801 -- no future respin needed, Tier-1 upstream
  `i2c_dw` -- per the E1M-AEN-2626-R2 netlist + `ADTS0013` v1.2 Table 3-16.
  R2-sourced; the bench module on hand is r1, so this still needs an on-unit
  probe (no R1 netlist is available).
- **SPI needs the SoC master-mode select set (not just CTRLR0).** The Ensemble
  wraps the DWC_ssi macrocell behind a SoC master/slave select in
  `CLKCTRL_PER_SLV.SSI_CTRL` (`0x4902F028`: bit n = SSI*n* master-mode, bit 8+n =
  SS value). Until it's set the controller ignores `CTRLR0.SSI_IS_MST` and never
  drives SCLK as a master (TX FIFO fills, `spi_transceive` → -116). Upstream
  Zephyr's alif SoC layer doesn't program it, so the alp-sdk driver does, in init
  (PR #162). Customers need nothing — it's automatic for master instances. The
  SPI node also carries `clock-frequency` (BAUDR divider) since the clock
  controller doesn't report a rate.
- **Ethernet DMA buffers must live in global SRAM0, not the M55 DTCM.** The long
  no-link was traced to the GMAC DMA descriptor rings + net_buf pool sitting in
  the M55 **DTCM** (`zephyr,sram = &dtcm`), which is **not** reachable on the GMAC
  DMA bus — so the MAC never saw valid descriptors. Fix: `zephyr,sram = &sram0`
  (global on-chip SRAM `0x02000000`, where the CPU address equals the DMA address)
  + `CONFIG_DCACHE=n`. The PHY power (`E_PHY_PWRDWN` = P15_4 lpgpio), PHY reset
  (`E_PHY_RESET` = P11_6 gpio11), and the RCSR bit7 `REF_CLK_SEL=1` ref-clock
  select were all already correct — the earlier "PHY RX path / `ANLPAR=0` / scope
  the REF_CLK" diagnosis was a red herring (a bad cable plus the DTCM starvation).
  Re-validated end-to-end: DHCP lease `192.168.10.137` (server-side dnsmasq lease
  + ARP `REACHABLE`).
- **Generalizable: any DMA-master block needs its buffers in global SRAM.** On the
  E8 M55, any DMA-master block (GMAC, the Ethos-U NPU, the SDHC) needs its
  DMA-visible buffers in global **SRAM0/SRAM1**, never the default DTCM.
- **Audio clocks (I2S bit clock + HP-PDM functional clock) are now in the Tier-1.5
  clockctrl west-patch, not in example pokes.** The upstream Alif clock controller
  (`drivers/clock_control/clock_control_alif.c`) only flips per-peripheral *gate*
  bits — it never enables the audio *master* source, so the I2S and PDM examples
  used to poke raw CGU / EXPMST0 addresses by hand. That is now folded into the
  clockctrl, carried as
  `zephyr/patches/zephyr/0001-clock_control_alif-master-source-expmst-i2s-setrate.patch`
  (`zephyr/patches.yml`, applied with `west patch apply`). On `clock_control_on()`
  of an I2S or HP-PDM clock the patched driver now: (a) **GROUNDED, bench-proven** —
  enables the CGU master **76.8 MHz / HFOSCx2** (`cgu_base + 0x14`, bit 24) and, for
  the EXPMST0-domain HP PDM, the `EXPMST0_CTRL` **IPCLK_FORCE/PCLK_FORCE** bits
  (`clkctl_per_slv_base + 0x00`, bits 31|30); and on `clock_control_set_rate()` it
  programs (b) **BENCH-UNVERIFIED** the `I2Sx_CTRL` bit-clock divider
  (`76.8 MHz / sclk` into the 10-bit `CKDIV` field) so `i2s_dw.c` stops getting
  `-ENOSYS`. The (b) divider field layout is taken from the Alif `i2s_sync`
  reference but is **not yet confirmed** against the DFP/TRM for the DesignWare
  `i2s_dw` path — it is a separable hunk in the patch so the bench step can land
  (a) and hold (b) if the achieved sample rate regresses. (The 76.8 MHz oscillator
  itself is SE-managed, so the PDM may still need the `se_services`/MHU clock
  request even with the CGU `CLK_ENA` bit set — see the PDM example's PARTIAL note.)

## 4. Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| `app-write-mram`: `Target did not respond` | SE-UART wiring/baud — 1.8 V adapter, crossed TX/RX, common GND, port = the FT232R SE-UART, baud 57600. |
| Image written but won't boot | ATOC built with the wrong **DEVICE** config — write an **app-only** ATOC keeping the factory DEVICE config. |
| `west flash` tries to use J-Link | The carrier defaults to the **`alif_flash`** runner (SETOOLS/ISP, now flow A / fallback). J-Link *does* burn MRAM (flow D) with the `AE822FA0E5597LS0_M55_HE` device profile — the loader is built into J-Link V9.46+, no separate pack. |
| No app output over USB | Expected — only the SE-UART is on USB. Use the RAM console (flow B) or RTT. |
| RAM console all-zeros | Read the **`ram_console_buf`** symbol (not `ram_console`); re-resolve from `zephyr.map`; ensure `CONFIG_UART_CONSOLE=n`. |
| J-Link `Could not connect to the target device` (Alif part device) | For **read/attach/RAM-run** use the generic `-device Cortex-M55` (attaches to the live core). For **MRAM flash** (flow D) the `AE822FA0E5597LS0_M55_HE` part device is required — it unlocks the built-in MRAM loader (J-Link V9.46+ DLL) (§ Flow D). |
| Link error `region FLASH overflowed` on a RAM-run app | The overlay used `zephyr,flash = <&itcm>` — use the path-reference form `&itcm` (else `FLASH_SIZE=0`). |
| I2C2 probe times out (`-ETIMEDOUT`) | Bus stuck — pads not driving. Add the I2C pinctrl pad config (§3): `input-enable` + `bias-pull-down`; run at 100 kHz. |
| I2C2 clean NACKs but no device ACKs | The pinctrl is missing **`input-enable`** (REN) so the controller can't sense SDA, or it used `bias-pull-up` (DSC=1) instead of `bias-pull-down` (DSC=2). Match Alif's reference (§3) — then the EEPROM ACKs at 0x50. |
| `spi_transceive` returns `-116` (TX FIFO full, no SCLK) | SoC master-mode not set — `CLKCTRL_PER_SLV.SSI_CTRL` (`0x4902F028`) per-instance master bit. The alp-sdk driver sets it in init (PR #162); if you forked the driver, replicate it. |
| `spi_transceive` returns `-EINVAL` with no register programming | No `clock-frequency` for the BAUDR divider and the alif clock controller has no `get_rate`. Set `clock-frequency` on the SPI node (§3). |
| Ethernet links but never gets a lease / no traffic | GMAC DMA descriptor rings + net_buf pool are in the M55 **DTCM** (`zephyr,sram = &dtcm`), off the DMA bus. Move them to global SRAM0: `zephyr,sram = &sram0` + `CONFIG_DCACHE=n` (§3). Applies to any DMA-master block (GMAC/NPU/SDHC). |
| I2S TX never clocks out / PDM `dmic_read` → `-EAGAIN` (FIFO=0) | The CGU master **76.8 MHz** source and (for the HP PDM) the `EXPMST0_CTRL` IPCLK/PCLK force bits are not set. These are now enabled by the Tier-1.5 clockctrl west-patch (`west patch apply`; §3) on `clock_control_on()` — confirm the patch is applied. The 76.8 MHz oscillator itself is SE-managed, so the PDM may also need the `se_services`/MHU clock request even with the CGU bit set. |
| I2S sample rate looks wrong (pitch off) | The `I2Sx_CTRL` `CKDIV` divider the clockctrl `.set_rate` programs is **BENCH-UNVERIFIED** (field layout from the Alif `i2s_sync` reference, not the DFP/TRM). Confirm the divider width/position + N-vs-(N-1) convention against the Alif DFP/TRM; the hunk is separable in the patch so it can be held. |
| **LPRTC** (`snps,dw-apb-rtc`, Tier-2) | ✅ PASS (RAM-run, 2026-06-17) | The always-on `lprtc@42000000` free-running 32-bit counter advances (delta 3467 ticks / ~100 ms at 32768 Hz) via the portable Zephyr counter API over the vendored `counter_dw_rtc` driver. `counter_start` returns `-EALREADY` → the **VBAT clock-gate is already on** (no `VBAT_LPRTC0_CLK_EN` write needed on the upstream-Zephyr build path). Fixed two driver bugs the link-only check missed: the missing `.get_top_value` (faulted PC=0x0) + `max_top_value`. It is a **counter**, not a calendar RTC — the `alp_rtc_*` calendar shim is still TBD. Example: `examples/aen/aen-rtc-regcheck`. |
