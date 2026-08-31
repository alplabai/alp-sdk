<!--
Copyright (c) 2026 Alp Lab AB
SPDX-License-Identifier: Apache-2.0
-->

# Secure-Enclave (SE) runtime services — E1M-AEN (Alif Ensemble)

How an M55 application talks to the **Secure Enclave (SES)** at *runtime* on an
**E1M-AEN** SoM — querying device identity / lifecycle, reading the power
profile, and (gated) changing DVFS or the secure-boot table. This is the
**`se_service_*` API** path, distinct from the one-time
[`aen-provisioning.md`](aen-provisioning.md) SES→MRAM flash flow.

> Bench-verified on a real **E1M-AEN801** (E8, RTSS-HE) — see the
> `aen-se-service-info` (#192), `aen-se-service-query` (#197) examples and
> [`bring-up-aen.md`](bring-up-aen.md).

> **Portable-first.** Application code does **not** call `se_service_*`
> directly — the SDK wraps the read-only + boot services behind portable
> surfaces backed by registry backends (`silicon_ref="alif:ensemble:e8"`):
> SoC identity via `alp_soc_info_read` / `alp_soc_secure_fw_ping`
> (`<alp/hw_info.h>`), RUN/STANDBY operating-point profiles via
> `alp_power_profile_get`/`_set` (`<alp/power.h>`), peer-core release via
> `alp_mproc_boot_core` (`<alp/mproc.h>`), and TRNG/crypto via
> `<alp/security.h>` (SE CryptoCell).  This doc remains the TRANSPORT +
> bring-up reference (what the backends and the vendor-scoped
> `aen-se-service-info` regcheck ride); `aen-se-service-query` shows the
> portable consumer path.

## 0. The model

The SES is always running on the SE core. The M55 reaches it over **two Arm
MHUv2 mailboxes** (`seservice0r` RX @0x40040000 IRQ 37, `seservice0s` TX
@0x40050000 IRQ 38), tied together by the `se_service` DT node. The Apache-2.0
hal_alif client (`se_services/zephyr/src/se_service.c`) drives them through
Zephyr's IPM API.

On the alp-sdk (upstream-Zephyr + hal_alif) stack you must enable the in-tree
glue — upstream Zephyr ships none of it:

```ini
CONFIG_ALP_SDK=y
CONFIG_ARM_MHUV2=y            # the in-tree arm,mhuv2 IPM driver
CONFIG_HAS_ALIF_SE_SERVICES=y # compiles hal_alif se_service.c
```

…plus the board overlay that okays `seservice0r` / `seservice0s` / `se_service`
and sets the `itcm`/`dtcm` `global_base` props `local_to_global()` needs. Copy
the overlay from `examples/aen/aen-se-service-info/boards/`. The SE answers even
on a J-Link RAM-run, so these examples validate without a flashed image.

Every `se_service_*` call bounds its wait internally (returns `0` / `-EAGAIN`
(timeout, retry) / `-EBUSY` (SE busy) / a positive SE error code), so a call
never hangs — if the SE is unreachable you get a bounded error, not a lockup.

### 0.1 Version pairing — the SERAM image and the services library must match

The SE firmware image is called **SERAM**. It is the thing
`se_service_get_se_revision()` reports (`SES A0 v1.110.0 Mar 4 2026`) — hal_alif
documents that call as "Retrieve SERAM version banner"
(`se_services/include/services_lib_protocol.h`) — and it is
independent of your application: it is programmed into the module, not built
from this SDK. The **services library** is the client half — the hal_alif
`se_services/zephyr/src/se_service.c` this SDK links, whose upstream is Alif's
SETOOLS release of the same number.

**These two are versioned together and are not independently upgradable.** Alif
Semiconductor, 2026-08-28, on an E8 (AE822) module:

> there is an API break between SERAM v106 and v109 for E8 devices. v106 is a
> really early version for E8 platform, and you definitely need to update SERAM
> on your HWs to a newer version (v110 is recommended). It works ok with also
> with services library v109. General guideline is that you should always use a
> matching SERAM and services library.

| SERAM on the module | Services library | Supported |
| --- | --- | --- |
| **v110** | v110 | **Yes** — Alif's recommendation, and the E1M-AEN801 reference baseline |
| v109 | v109 | Yes |
| **v106** | v109 / v110 | **No** — straddles the API break; v106 is, in Alif's words, "a really early version for E8" |
| any | any other number | No — Alif's general guideline is to match them |

Alif name these versions by the middle number of the SES string: SES
`1.110.0` is SERAM **v110**, `1.106.2` is **v106**. That mapping is our
reading of their reply, not something they spelled out — if you are quoting a
version back to Alif, quote the full string.

**Check what a module is running before you debug anything else**, over the
already-safe read-only path of §1:

```c
uint8_t rev[80];
se_service_get_se_revision(rev);   /* "SES A0 v1.110.0 Mar 4 2026" */
```

A module below v109 must have its SERAM updated — a **System Package update**
over the SE-UART with SETOOLS. Changing the application cannot fix a mismatched
pair; it can only move the symptom.

**Symptom seen on a mismatched pair.** On a customer AE822 running SERAM
**1.106.2** (Jul 14 2025) against a services library from SETOOLS **1.109**, the
*first* Secure Enclave service request stops **HFXTAL** and unlocks the **PLL**:
the M55-HP drops from **400 MHz** to **76.8 MHz** and stays there. It reproduced
through `SERVICE_CRYPTOCELL_GET_RND` (reached from a DHCP client's randomness
source) but is not RNG-specific — shifting application code by `0x100` changes
the outcome, which points at the first-request path rather than at the service.
The healthy E1M-AEN801 reference board, on matched v1.110.0 / SETOOLS 1.110.00,
does not reproduce it (#1700).

Whether the break *causes* that particular fault is the wrong question to wait
on. **Across an API break any behaviour is permissible** — a clean error, a
wrong answer, or hardware left in a state neither side intended. Nothing you
measure on a mismatched pair is evidence about anything, a workaround built on
one is not safe to ship even when it appears to work, and the next mismatched
module can fail differently. Fix the pairing first; triage after.

**Alp Lab's position: E1M-AEN tracks Alif's latest released SERAM**, currently
**v110**, with the services library re-pinned alongside it — see
[ADR 0030](adr/0030-aen-seram-tracks-alif-latest-as-a-matched-pair.md). If you
run an E1M-AEN module below that floor, update it; do not wait for a symptom.

> **Open question — which SERAM floor does this SDK's pin require?** alp-sdk
> pins `hal_alif v2.3.0` (`west.yml`), the newest `hal_alif` release, which
> supplies the services library. You cannot read the answer off the pin: that
> library versions itself independently of SETOOLS — `services_lib_protocol.h`
> declares `SE_SERVICES_VERSION_STRING "0.50.10"`, which is not a number
> comparable to SERAM v106/v109/v110. Only Alif can supply the mapping, so the
> module baseline is **v110** on the strength of their recommendation and the
> reference board, not on a published correspondence.

## 1. Read-only services — safe, zero-risk

These never change device state. All confirmed `rc=0` on the E8 bench (#197):

| Service | Returns | E8 bench value |
| --- | --- | --- |
| `se_service_heartbeat()` | liveness | rc=0 |
| `se_service_get_se_revision(u8 buf[80])` | SERAM (SE firmware) version string | `SES A0 v1.110.0 Mar 4 2026` — was `SES A0 v1.106.2 Jul 14 2025` before the bench System Package update; see §0.1 |
| `se_service_get_toc_number(u32*)` | TOC entry count | 5 |
| `se_service_get_toc_version(u32*)` | TOC version | `0x016a0200` |
| `se_service_get_device_part_number(u32*)` | part id | `0x000002a0` |
| `se_service_system_get_device_data(get_device_revision_data_t*)` | LCS + ids + keys digests + serial | LCS `0x01` (**DM**), `ALIF_PN="AE822FA0E5597LS0"` |
| `se_service_get_run_cfg(run_profile_t*)` | live power/clock profile | DCDC **825 mV**, `power_domains=0x16d`, `cpu_clk_freq=4`, `run_clk_src=2` |
| `se_service_get_off_cfg(off_profile_t*)` | standby/wake profile | DCDC 825 mV, no wake/EWIC configured |
| `se_service_get_rnd_num(u8*, len)` | SE TRNG bytes | 8 random bytes |

`se_service_get_last_set_run_cfg()` returns the cached run profile without an SE
round-trip (faster).

### SERAM version, per physical board (#1797)

ADR 0030 sets the supported SERAM floor for E1M-AEN at **v110**, and says a
board below it "is updated before it is trusted to produce evidence". There is
more than one AEN board in play, so the version has to be recorded per board,
not once for the family.

| Board | SEROM | SES / SERAM | LCS | Meets the v110 floor? | Captured |
|---|---|---|---|---|---|
| The AEN bench unit (`AE822FA0E5597LS0`, module rev **r1**, serial `2617-0001`) | `v1.105.65 0x000002A0` | `SES A0 v1.110.0 Mar  4 2026`, SERAM0/SERAM1 `1.110.0` | `1` (DM) | **Yes** | 2026-08-30, re-confirmed 2026-08-31 |

**There is only one AEN board.** This table used to carry a second row for "the
second AEN bench board (on the internal board farm)", marked `TBD` /
**Unverified**, and #1797 existed largely because of it. That row described
hardware that does not exist. The board-farm place `e1m-aen-evk-01` is a stale
definition: every resource on it reports `avail: False` with `port: None` and
`path: None`, the exporter host has no `/dev/ttyUSB*` at all, and the exporter's
own udev match rules name `ID_SERIAL_SHORT` `BG01028C` and `BG04503B` — the same
two FTDI adapters this bench enumerates as its SE-UART and app-console ports. One
board, moved off the farm. Retire or re-point that place if the board ever goes
back.

The capture is the SES boot header on the SE-UART at 57600 8N1 — it streams on
every reset, no app or SE service call needed:

```
SEROM v1.105.65 0x000002A0
SES A0 v1.110.0 Mar  4 2026 19:05:34
[SES] SERAM bank 0x0 is valid and booted
[SES] LCS=1
| * SERAM0 |  CM0+  | ---------- | 0x000000C0 | ...  |    64528 |    1.110.0| ------ |
|   SERAM1 |  CM0+  | ---------- | 0x00020AC0 | ...  |    64528 |    1.110.0| ------ |
```

That is the cheapest way to answer "is this board above the floor" for any AEN
unit: power-cycle it with the SE-UART open and read the first two lines.

**Two other facts fall out of the same header and are worth keeping here.**

`[SES] No LF XTAL` — this unit has no low-frequency crystal detected, so the
LPRTC runs from the internal LFRC rather than the 32 kHz LFXO. That compounds
the RTC errata recorded in `src/backends/rtc/lprtc_calendar_shim.c`: AERR0012
ER002 describes the LFXO-to-LFRC fallback as a transient during POR_N, but on a
board with no LF XTAL at all it is the steady state. Treat LPRTC calendar
accuracy on this unit as LFRC-grade (Alif quote ~5% offset from LFXO) at all
times, not only across a reset. See #1814.

`[SES] SE frequency is 78.31 MHz` — an independent corroboration of the ADC
clock measurement in #1823, which derived `ADC_CLK` at `clock_div = 2` as
~78.3 MHz and therefore `PERIPH_CLK` ~156.5 MHz. The SE reporting 78.31 MHz for
itself lands on the same divide-by-two of the same ~156.6 MHz source, from a
completely different path.

**Lifecycle-state (LCS) legend:** `0x0` CM (chip mfr) · `0x1` **DM** (device
mfr, the maker-provisioned state) · `0x5` SE (secure-enabled) · `0x7` RMA.

## 2. Mutating services — GATED (recovery required before any bench run)

These change live device state. **Do not run them on a single bench board
without a sacrificial unit and a proven SETOOLS recovery path** (§3). They are
documented here so the path is review-ready, not so it is run casually.

### 2.1 `se_service_set_run_cfg(run_profile_t*)` — DVFS / power

Sets the run power/clock profile (DCDC voltage 750–850 mV, clock sources, CPU
frequency, power domains, memory retention, IO-flex 3V3).

```c
run_profile_t p;
se_service_get_run_cfg(&p);     /* read the live baseline first */
p.cpu_clk_freq = <target>;      /* change ONE field for DVFS    */
int rc = se_service_set_run_cfg(&p);
```

**Two traps:**

1. **Cache short-circuit.** `set_run_cfg` *skips the SE call entirely if the
   profile equals the cached value* — so re-asserting the value `get_run_cfg`
   just returned is a **no-op that returns `rc=0` without any SE round-trip**.
   It validates nothing about the SE set path; it only proves the wrapper's
   cache logic. To actually exercise the SE you must change a field — which
   changes the live operating point.
2. **Brownout risk.** A `dcdc_voltage` / `cpu_clk_freq` / `run_clk_src` the rail
   can't sustain browns out or hangs the core. Recovery is a **power cycle**
   (the operator must do it physically — a J-Link `loadbin` reset does not
   restore a collapsed rail). Always read → change one field → `set` → re-read
   to confirm → keep the baseline to restore.

`se_service_set_off_cfg()` is the standby twin; same caution.
`se_service_clock_set_divider()` changes a PLL/bus divider directly — same
brownout class.

### 2.2 `se_service_boot_cpu(cpu_id, entry_addr)` — peer-core release

Backs `alp_mproc_boot_core()` (`<alp/mproc.h>`, `src/backends/mproc/alif_se_boot.c`).
Asks the SES to release a peer M55 at `entry_addr` -- per the vendor manual
(`SE_Host_Services_API_v1.109.0.pdf` p.112, `SERVICES_boot_cpu`) it "does not
perform image loading, verification, etc., it just boots the core"; residency
at `entry_addr` is the caller's responsibility, arranged before this call.

> **Direction-specific failure (E8, `AE822FA0E5597LS0`, 2026-07-31):** a plain
> `"flags": ["load"]` ATOC entry + `se_service_boot_cpu()` **works** in the
> HP-master → HE-peer direction -- `ALP-HE` read `uLV` (Loaded, Verified) with
> Dest Addr `0x58000000` populated (28.52 ms load time), the bytes at
> `0x58000000` matched the staged binary on a repeat read, and the link
> carried 16/16 PING/PONG round-trips. The **same recipe fails** in the
> HE-master → HP-peer direction: the ATOC still reported `uLV`, but two
> independent debug access ports read the HP peer's ITCM as uninitialized
> SRAM from t+0.80s to t+60s. Releasing that core vectors from garbage and
> locks up immediately (`CFSR = 0x00000101`, `PC = 0xEFFFFFFE`).
>
> **Root cause is vendor-documented, not an inaccurate `uLV`.** The SES does
> place the bytes at ATOC-processing time in both directions; M55-HP's TCM is
> separately invalidated by the reset `SERVICES_boot_cpu()` issues before
> release, so HP's TCM is empty again by release time even though the SES
> table (correctly, as of load time) still says `uLV`. Two vendor passages
> describe this and **disagree on device scope**:
> - p.112, `SERVICES_boot_cpu`: "For the M55 cores, there are cases in which
>   this service does not work. The currently known case is the **M55-HP
>   core in FUSION REV_Bx devices**, where resetting the core also
>   invalidates its TCM content."
> - p.115, `SERVICES_boot_release_cpu`: "A known case is the **M55-HP core
>   in Ensemble devices**. Because of that, after calling
>   `SERVICES_boot_reset_cpu()` to stop the core, the image in the TCM must
>   be reloaded, before calling `SERVICES_boot_release_cpu()`."
>
> E8 is Ensemble, so the second passage applies here. M55-HE is not
> documented with this erratum, matching the working direction above.
>
> The fix for the HE-master → HP-peer direction is deferred ATOC processing
> -- `"flags": ["load", "boot", "deferred"]` (`"deferred"` is a flags-array
> member, not a sibling boolean key) sets `TOC_IMAGE_DEFERRED`, the SES skips
> the boot-time release, and the host reloads + releases at runtime with
> `SERVICES_boot_process_toc_entry` (service 500) -- the vendor manual
> (p.112) calls 500 the "higher-level ... convenient way to boot a CPU core"
> and recommends it over 501 for M55 cores generally. That combination
> carried an RPMsg link through 495 consecutive PING/PONG round-trips over
> 4m11s, and 16/16 on the in-tree `aen-rpc-pingpong` example. This backend
> now implements that path too, gated by
> `CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC` (default ON only when the
> configured peer is HP -- `CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC_PEER_IS_HP`,
> see the Kconfig help in `zephyr/kconfigs/mproc-rpc-usb.kconfig`) -- but
> every shipped example still rides service 501 as-is: all six that call
> `alp_mproc_boot_core()` (`aen-alp-rpc`, `aen-rpc-pingpong`,
> `aen-dualcore-ipc`, `aen-dualcore-doorbell`, `aen-dualcore-master`,
> `firmware-update-log`) release **HE from an HP master**, the working
> direction for the plain path, and none of them set `PEER_IS_HP`. The
> `aen-dualcore-he-master` example is the in-tree consumer of the deferred
> path (`examples/aen/aen-dualcore-he-master/testcase.yaml`), silicon-proven
> releasing an HP peer via a cold-cycle bench run. Full writeup: the
> precondition comment on `alif_se_boot_core()` and
> `docs/aen-bench-bringup.md` § Flow A — Dual-core deferred-TOC boot.
>
> **Open:** whether `entry_addr` is honoured by service 501 at all was not
> isolated by either run above -- the entry point came from the ATOC in
> both cases.

### 2.3 `se_service_update_stoc(u8 *img, u32 size)` — A/B secure-boot update

Rewrites the **System TOC (STOC)** in MRAM — the customer secure-boot / A-B
field-update path. This is the most destructive SE service: a malformed STOC
leaves the SES with nothing valid to boot → **the module does not boot → brick**,
recoverable only over the SE-UART with SETOOLS (§3), and not always recoverable.

It ties into the SoM-maker provisioning model (the module ships DM-provisioned
with a dev-signed ATOC; see `aen-provisioning.md`) and is the SES-native answer
to customer secure-boot + A/B that the
[`aen-bench-bringup.md`](aen-bench-bringup.md) MCUboot analysis lands on — the
SES verifies the signed slot content cert, so the field-update is a *STOC swap*,
not a software-MCUboot chainload.

```c
/* img/size = a SETOOLS-built, signed STOC image staged in RAM/MRAM. */
int rc = se_service_update_stoc(stoc_img, stoc_size);
/* rc=0 => SES accepted + wrote the new STOC; next boot uses it. */
```

**Do not bench this without:** (a) a sacrificial board, (b) the SETOOLS recovery
chain (§3) proven on that board first, (c) a known-good STOC image to roll back
to. Until then it stays design-only.

### 2.4 Also mutating (out of scope here)

`se_service_boot_es0` / `shutdown_es0` (power a subsystem; needs an NVDS config
blob), `se_service_se_sleep_req` (clears the SE-ready flag — next call
re-syncs), `se_service_boot_reset_soc` / `boot_reset_cpu` (reset),
`se_service_system_set_services_debug` (changes the SE debug posture). All change
state; none are needed for the read-only characterisation.

### 2.5 `SERVICE_APPLICATION_OSPI_WRITE_KEY_ID` (service 105) — OSPI SecAES key provisioning

Issue #224: this was deferred as "blocked on the vendor HAL pack" -- wrongly.
The E8 HWRM's `AES_CONTROL` description makes `AES_CONTROL_LD_KEY` (hal_alif
`drivers/ospi/include/ospi.h`) the bit the **SE** arms once it has written the
key; the host never pokes raw key bytes into an OSPI register (no code in
`drivers/ospi/src/ospi.c` ever touches that bit -- it isn't the host's job).
The host-side entry Alif publish for this is `AUGD0014` "SE Host Services
API" v1.109.0's `SERVICES_application_ospi_write_key(handle, command, key,
error_code)`, with `OSPI_WRITE_OTP_KEY_OSPI0/1` and
`OSPI_WRITE_EXTERNAL_KEY_OSPI0/1` command codes
(`se_services/include/services_lib_api.h`).

That symbol's *body* is not compiled anywhere in the Apache-2.0 hal_alif
module pinned by this SDK (v2.3.0) -- `se_services/` ships only
`se_service.c`, and every low-level `SERVICES_*` prototype in
`services_lib_api.h` (pinmux, pad control, uart_write, ospi_write_key, dmpu,
verify_image, ...) is declared, never defined, there; only the higher
`se_service_*` wrappers get bodies. So `src/backends/ext/alif/storage.c`
does not call that symbol -- it builds the identical wire packet
(`ospi_write_key_svc_t`, `services_lib_protocol.h`;
`SERVICE_APPLICATION_OSPI_WRITE_KEY_ID`, `services_lib_ids.h`) by hand and
hands it to the *public* generic transport hal_alif does export for exactly
this shape of caller:

```c
ospi_write_key_svc_t pkt;
memset(&pkt, 0, sizeof(pkt));
pkt.header.hdr_service_id = SERVICE_APPLICATION_OSPI_WRITE_KEY_ID;
pkt.send_command          = OSPI_WRITE_EXTERNAL_KEY_OSPI0; /* R2 BOM: OSPI0 only */
memcpy((void *)pkt.send_key, key, sizeof(pkt.send_key));   /* fixed 16 B -- AES-128 only */
pkt.resp_error_code       = UINT32_MAX;                    /* sentinel -- see below */
int rc = se_service_send_request((uint32_t *)&pkt, (uint32_t)sizeof(pkt));
/* rc==0 && pkt.header.hdr_error_code == SERVICES_REQ_SUCCESS
 *       && pkt.resp_error_code == OSPI_WRITE_KEY_SUCCESS => the SE wrote the key */
```

`se_service_send_request()` is the same public seam
`src/backends/security/se_cryptocell.c` rides for its CryptoCell AES/SHA/AEAD
packets (added by
`zephyr/patches/hal_alif/0002-se-service-add-public-send-request.patch`),
over the identical MHUv2 mailbox this doc's §1 read-only path is bench-proven
on. `pkt.send_key` is an **inline** 16-byte field in the request packet (not
a DMA address like the CryptoCell key fields) -- it is the one place the SDK
holds the key in clear, and the caller (`alp_alif_storage_secaes_key_provision`)
wipes the whole packet with a compiler barrier immediately after the call
returns, success or failure.

**Both verdict fields are checked, not just one.** `send_msg_to_se()` (hal_alif
`se_service.c`) returns `rc==0` as soon as *any* response frame arrives over
the mailbox -- it never itself inspects `header.hdr_error_code` or
`resp_error_code`, and no `se_service_*` wrapper in hal_alif does either. A
first cut of this backend checked only `resp_error_code != OSPI_WRITE_KEY_SUCCESS`,
which is a **false-`ALP_OK` bug**: `OSPI_WRITE_KEY_SUCCESS` is `0x0`, and the
packet is `memset` to 0 before the send, so an SE that answers with a
transport-layer NACK (e.g. `SERVICES_RESP_UNKNOWN_COMMAND` if this firmware
simply doesn't implement service 105 at all) and never touches
`resp_error_code` would read back indistinguishable from success. The fix
pre-seeds `resp_error_code` to a non-success sentinel (`UINT32_MAX`) before
the send and checks `header.hdr_error_code != SERVICES_REQ_SUCCESS` as well
-- so a provisioning tool cannot report success while the OSPI AES slot was
never keyed.

**The call blocks -- for up to ~35 s -- and is bracketed against a
concurrent close.** `se_service_send_request()`'s `se_service_ensure_ready()`
+ `svc_mutex` each carry hal_alif's 15 s `MUTEX_TIMEOUT`, plus a 10 s
`SERVICE_TIMEOUT`. `alp_alif_storage_secaes_key_provision()` /
`_get_status()` both enter via `alp_handle_op_enter()` before touching the
handle and leave via `alp_handle_op_leave()` on every exit path (issue #629's
open/op/close guard, `src/common/alp_slot_claim.h`) -- the same pairing
`alp_storage_close()` drains on, so a close racing an in-flight provision
call blocks until it returns instead of freeing/recycling the handle out
from under it.

**Key width is fixed at 16 bytes (AES-128).** `OSPI_KEY_LENGTH_BYTES` in
`services_lib_api.h` and `ospi_write_key_svc_t.send_key`'s array size both
say 16 -- there is no 192/256-bit OSPI SecAES key option in this service,
unlike the portable (still-unimplemented) `alp_storage_configure_inline_aes`
surface.

**No status read-back exists.** Alif has not published an SE service that
reads the OSPI SecAES engine's ARMED/ENGAGED/error state back --
`alp_alif_storage_secaes_get_status()` stays `ALP_ERR_NOSUPPORT`
unconditionally. The host-side `ospi_aes_regs` register block is the SE's
own domain for the `LD_KEY` sequence; reading it from the host without a
vendor-confirmed sequence would repeat the exact "guess the register timing"
mistake issue #224 was reopened to avoid.

**SES version.** No document available here states a per-service SES floor
for this call specifically (`AUGD0014` is Alif-confidential; the passages
above are paraphrased, not quoted). §0.1's general SES **v110** floor
(ADR 0030) is what this SDK tracks; the bench unit's `SES A0 v1.110.0`
already exceeds both it and `AUGD0014`'s own v1.109.0 doc revision.

**Gated `CONFIG_ALP_SDK_STORAGE_ALIF_SECAES`, default OFF.** About eight
in-tree AEN examples already set `CONFIG_HAS_ALIF_SE_SERVICES=y`
(`aen-aipm-read`, `aen-alp-rpc`, `aen-dualcore-doorbell`/`-ipc`/`-master`/
`-he-master`, `aen-rpc-pingpong`, `aen-se-service-*`), which would otherwise
turn this real SE transaction on by default for all of them. Unlike
`CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL_SEND_SEAM` (default ON *because* that
path bench-PASSED, `docs/aen-bench-bringup.md`), this gate stays OFF until
the round-trip below is proven; flip the default in the same change that
adds a bench PASS row.

**UNVERIFIED ON SILICON.** No bench unit reachable at implementation time has
an OSPI SecAES-relevant part populated -- the bench module is board rev r1
(EEPROM manifest "E1M-AEN801 r1", serial `2617-0001`); the Macronix
`MX25UM25645GXDI00` this targets is populated (`DNP = 0`) only on the R2 BOM
(#915). The transport half of this call reuses the identical
`se_service_send_request()` path already bench-proven for SE CryptoCell
(`aen-se-crypto`: SHA-256 + AES-128-GCM MATCH); the OSPI write-key
round-trip itself has not been run against real SE firmware.

## 3. Recovery (a bad STOC / collapsed rail)

- **Collapsed rail (bad `set_run_cfg`/divider):** physically power-cycle the
  board, then re-flash via J-Link RAM-run or the SES path. The bad profile is
  not persisted unless you also wrote it to an off/boot config, so a power cycle
  returns the default.
- **Bad STOC (`update_stoc`):** the M55 will not come up. Recover over the
  **SE-UART** with **SETOOLS** — re-provision a known-good STOC
  (`maintenance` / `app-write-mram` per `aen-provisioning.md`). Validate this
  chain on a sacrificial board *before* ever calling `update_stoc` on a unit you
  care about.

## 4. Bench-execution policy (summary)

| Service class | Bench-runnable now? |
| --- | --- |
| §1 read-only queries | **Yes** — zero risk, validated on E8 (#197) |
| §2.1 `set_run_cfg` (real change) | No — needs power-cycle recovery on hand; idempotent re-assert is a cache no-op |
| §2.2 `boot_cpu` (peer-core release) | **Yes** for HP-master → HE-peer with plain `["load"]`; HE-master → HP-peer needs `["load","boot","deferred"]` — a plain `["load"]` entry locks the HP peer up (see §2.2) |
| §2.3 `update_stoc` | No — sacrificial board + proven SETOOLS recovery required first |
| §2.5 `ospi_write_key` (OSPI SecAES key provisioning) | No — every bench unit reachable here is board rev r1 with no OSPI SecAES-relevant part populated; the code path is real (`CONFIG_ALP_SDK_STORAGE_ALIF_SECAES`) but its silicon round-trip is unverified |

See `examples/aen/aen-se-service-info` (vendor-scoped transport + LCS regcheck)
and `aen-se-service-query` (the read-only surface via the portable `alp_*`
wrappers) for the runnable parts.
