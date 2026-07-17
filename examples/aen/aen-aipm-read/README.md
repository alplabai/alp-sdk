# aen-aipm-read -- read the RUN/STANDBY operating-point profiles (portable)

Read the **live power/clock operating-point profiles** on the
**E1M-AEN801** (Ensemble E8, M55-HE) through the portable
`<alp/power.h>` profile surface, and print every portable field.
**READ-ONLY** -- this app never changes the operating point.

On the Ensemble E8 the profile store behind the portable surface is
the Secure Enclave's **aiPM** (Autonomous Intelligent Power
Management): the SE -- not the M55 -- owns the power/clock tree, and
the SDK's registry backend rides the SE-service mailbox.  The example
code itself carries **no vendor include** -- that is the point.

## What it reads

| Call | What it carries |
|------|-----------------|
| `alp_power_profile_get(ALP_POWER_PROFILE_RUN, ...)` | Active operating point: CPU clock (**Hz** -- the backend maps the controller's ordinal encoding), core DC-DC rail (**mV**), on `power_domains`, retained `memory_blocks`, flexible-IO rail (mV). |
| `alp_power_profile_get(ALP_POWER_PROFILE_STANDBY, ...)` | Standby/off operating point + wake config: standby clock (Hz), rail mV, retained domains/memory, `wake_events`. |

The `power_domains` / `memory_blocks` / `wake_events` bitmasks are
**implementation-defined** -- their bit legends are SoC business
(documented in the per-SoM HW reference), so the app prints them raw
and portable code treats them as opaque tokens (log them, compare
them, read-modify-write them -- never construct them from portable
constants).

`alp_soc_secure_fw_ping()` (`<alp/hw_info.h>`) runs first as the
liveness gate, proving the controller answers before the profile
reads are trusted.

## Safety -- READ-ONLY by construction

Only the non-mutating side of the surface is called:

- `alp_soc_secure_fw_ping()` -- liveness ping
- `alp_power_profile_get()` -- read RUN + STANDBY profiles

The mutating companion `alp_power_profile_set()` is **deliberately
never invoked** -- a wrong rail/clock value can brown out the rail or
stall the core; profile writes need known-good target values from the
per-SoM HW reference + a recovery plan before any bench run (see the
warning on `alp_power_profile_set` in
[`include/alp/power.h`](../../../include/alp/power.h)).

Both getters ride a bounded controller-mailbox round-trip, so the app
never hangs.

## How it reaches the hardware

- `prj.conf` sets `CONFIG_ALP_SDK=y` and enables
  `CONFIG_ARM_MHUV2=y` (the in-tree `arm,mhuv2` IPM driver) +
  `CONFIG_HAS_ALIF_SE_SERVICES=y` (the hal_alif SE client the
  backend consumes).  With those on,
  `CONFIG_ALP_SDK_POWER_PROFILE_ALIF_SE` and
  `CONFIG_ALP_SDK_SOC_INFO_ALIF_SE` default on and register the E8
  backends at `silicon_ref="alif:ensemble:e8"` -- the dispatcher
  picks them over the wildcard NOSUPPORT stubs.
- `boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay` enables the RTSS-HE
  <-> SE MHUv2 mailbox pair (`seservice0r` @ 0x40040000,
  `seservice0s` @ 0x40050000) + the `se_service` root node, and
  retargets ROM to ITCM for the bench RAM-run.

On builds without a profile-capable backend (native_sim, SoMs whose
power tree the application core owns directly) the same code links
and runs; the calls report `ALP_ERR_NOSUPPORT` and the app says so.

## Build + bench (serial bench, not part of this commit)

This is a bench RAM-run with the RAM console (the app UART is
not on USB on this bench):

```sh
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
  examples/aen/aen-aipm-read
```

Then RAM-run via the bench helper and read `ram_console_buf`
over SWD (same flow `aen-se-service-query` uses). Expected tail:

```
RESULT PASS: portable profile surface returned RUN+STANDBY read-only
(RUN ... MHz @ ... mV; STANDBY ... mV) -- alp_power_profile_set never
called
```

`RESULT FAIL` means the ping or a profile read did not return
`ALP_OK` (`NOSUPPORT` = no profile-capable backend on this build;
`NOT_READY` = controller asleep/unreachable on this boot) -- see the
per-call `rc` above.

## Known / TBD

- **Expected profile values are bench facts, not invented here.**
  The actual CPU clock / rail mV / domain + memory masks a
  maker-provisioned E8 reports are whatever the controller returns
  on the bench -- this app prints them, it does not assert any
  specific value.
- **Vendor-legend decode moved behind the SDK.**  The previous
  revision of this example decoded the vendor enum ordinals
  (clock-source names, `PD_*` / `MB_*` bit legends) in application
  code, which required `<se_service.h>` in the app.  That decode now
  lives in the SDK's E8 backend (`src/backends/power/alif_se_profile.c`
  maps ordinals to Hz/mV); the per-bit domain/memory legends are
  per-SoM HW-reference material rather than portable API.

## See also

- [`aen-se-service-query`](../aen-se-service-query/) -- the broader
  read-only surface (SoC identity + entropy + both profiles) over
  the portable API.
- [`aen-se-service-info`](../aen-se-service-info/) -- the
  vendor-specific SE transport bring-up regcheck (the sanctioned
  escape hatch this example no longer needs).
- [`include/alp/power.h`](../../../include/alp/power.h) -- the
  portable sleep-mode + operating-point-profile surface.
