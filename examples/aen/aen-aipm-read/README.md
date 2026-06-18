# aen-aipm-read -- read the SE aiPM RUN/OFF power+clock profiles

Query the Secure Enclave (SE) for the **live aiPM** (Autonomous
Intelligent Power Management) profiles on the **E1M-AEN801**
(Ensemble E8, M55-HE), decode every numeric selector into a
human-readable legend, and print them. **READ-ONLY** -- this app
never changes the operating point.

It is the aiPM-focused companion to
[`aen-se-service-query`](../aen-se-service-query/): same proven,
non-mutating SE mailbox path, but it leans on the two aiPM
*getters* and turns the raw enum ordinals they return into named
output.

## What it reads

On the Ensemble E8 the SE -- not the M55 -- owns the power/clock
tree. A core reads/writes two profiles over the SE-service
mailbox:

| Profile | Getter | What it carries |
|---------|--------|-----------------|
| **RUN** (`run_profile_t`) | `se_service_get_run_cfg()` | Active operating point: HF clock source (HFRC/HFXO/PLL), CPU frequency selector (60..800 MHz), AON LF clock (LFRC/LFXO), DC-DC output mV + mode, on `power_domains`, retained `memory_blocks`, IP/PHY clock-gating, flex-GPIO rail. |
| **OFF** (`off_profile_t`) | `se_service_get_off_cfg()` | Standby/off operating point + wake config: standby HF source, DC-DC mV, retained domains/memory, `wakeup_events`, `ewic_cfg`, the `vtor_address` the core resumes at. |

The app decodes:

- **clock sources** -- `run_clk_src` / `stby_clk_src`
  (`hfclock_t`: HFRC / HFXO / PLL) and `aon_clk_src`
  (`lfclock_t`: LFRC / LFXO);
- **CPU frequency** -- `cpu_clk_freq` (`clock_frequency_t`,
  a *non-monotonic* ordinal: 60 MHz is ordinal 7, 100 MHz is
  ordinal 8) into its MHz string;
- **power domains** -- `power_domains` bitmask into the `PD_*`
  alias names (VBAT_AON, RTSS_HE, SRAMS, SYSTOP, ...);
- **memory blocks** -- `memory_blocks` bitmask into the `MB_*`
  names (SRAM0..9, ITCM/DTCM retention banks, MRAM, OSPI, SERAM, ...);
- **DC-DC** -- `dcdc_voltage` (mV) + `dcdc_mode`
  (OFF/PFM_AUTO/PFM_FORCED/PWM) and the flex-GPIO `vdd_ioflex_3V3`
  rail (3.3 V / 1.8 V).

## Safety -- READ-ONLY by construction

Only **non-mutating** services are called:

- `se_service_heartbeat()` -- liveness ping
- `se_service_get_run_cfg()` -- read RUN profile
- `se_service_get_off_cfg()` -- read OFF profile

The mutating side is **deliberately never invoked** -- a wrong
clock/voltage value can brown out the rail, stall the core, or
change the wake posture, and needs a recovery plan + explicit
sign-off before any bench run:

- `se_service_set_run_cfg()` / `se_service_set_off_cfg()`
- `se_service_clock_set_divider()` / `*_set_enable()`
- `se_service_update_stoc()` / `boot_*` / `*_sleep_req()`

Both getters bound their wait inside `se_service.c` (return
`0` / `-EAGAIN` / `-EBUSY` / a positive SE error), so the app
never hangs.

## How it talks to the SE

Same transport as `aen-se-service-query`:

- `prj.conf` sets `CONFIG_ALP_SDK=y` and enables
  `CONFIG_ARM_MHUV2=y` (the in-tree `arm,mhuv2` IPM driver) +
  `CONFIG_HAS_ALIF_SE_SERVICES=y` (the hal_alif SE client,
  Apache-2.0, consumed as a module -- it exposes
  `se_service_get_run_cfg` / `se_service_get_off_cfg`).
- `boards/alp_e1m_aen801_m55_he.overlay` enables the RTSS-HE
  <-> SE MHUv2 mailbox pair (`seservice0r` @ 0x40040000,
  `seservice0s` @ 0x40050000) + the `se_service` root node, and
  retargets ROM to ITCM for the bench RAM-run (plus the
  `itcm`/`dtcm` `global_base` props `local_to_global()` needs).

The `run_profile_t` / `off_profile_t` structs and every
`CLK_SRC_*` / `PD_*` / `MB_*` / `CLOCK_FREQUENCY_*` token come
from the vendor `aipm.h`, which `se_service.h` pulls in via
`services_lib_api.h`. The app does not re-declare them.

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
RESULT PASS: SE returned the aiPM RUN+OFF profiles read-only
(RUN clk=..., ... MHz, ... mV; OFF clk=..., ... mV) -- no
set_run_cfg / set_off_cfg called
```

`RESULT FAIL` means the SE did not answer one of
heartbeat / get_run_cfg / get_off_cfg with `rc=0` (SE
asleep/unreachable on this boot, or a service unsupported) --
see the per-call `rc` above.

## References

Transcribed (not vendored -- the Alif DFP is proprietary), each
value cited inline in `src/main.c`:

- aiPM types + enums: Alif DFP `se_services/include/aipm.h`
  (`run_profile_t` / `off_profile_t`, `lfclock_t` / `hfclock_t`,
  `clock_frequency_t`, `power_domain_t` + `PD_*`, `memory_block_t`
  + `MB_*`, `dcdc_mode_t`, `ioflex_mode_t`).
- SE-service request semantics: Alif DFP
  `se_services/source/services_host_power.c`
  (`SERVICES_get_run_cfg` -> `SERVICE_POWER_GET_RUN_REQ_ID`,
  `SERVICES_get_off_cfg` -> `SERVICE_POWER_GET_OFF_REQ_ID`).
- Zephyr client wrappers (Apache-2.0): hal_alif
  `se_services/zephyr/include/se_service.h`
  (`se_service_get_run_cfg` / `se_service_get_off_cfg`).

## Known / TBD

- **`memory_block_t` SoC variant.** The decode table targets the
  *default* `memory_block_t` layout in the hal_alif module
  `aipm.h` (the `#else` branch, used for the E8 since it is
  neither `CONFIG_SOC_SERIES_E1C` nor `CONFIG_SOC_SERIES_B1`).
  If a future E8 SE-service build is compiled against a different
  variant, the per-bit *names* shift; the raw `0x...` mask is
  always printed so any divergence is visible. The integrator
  should confirm the linked variant against the part's SE
  firmware.
- **Expected profile values are bench facts, not invented here.**
  The actual RUN clock source / CPU frequency / DC-DC mV / domain
  + memory masks a maker-provisioned E8 reports are whatever the
  SE returns on the bench -- this app prints them, it does not
  assert any specific value.

## See also

- [`aen-se-service-query`](../aen-se-service-query/) -- the
  broader read-only SE surface (se_revision / TOC / device-data /
  run-cfg / off-cfg / rnd); this app drills into the aiPM
  run/off profiles with full decode.
- [`include/alp/power.h`](../../../include/alp/power.h) -- the
  portable `alp_power_*` sleep-mode abstraction (RUN / SLEEP /
  DEEP_SLEEP / STANDBY). This example stays direct-to-SE because
  it reports the *vendor* aiPM profile detail (clock source,
  exact MHz selector, domain/memory masks) that the portable API
  intentionally abstracts away.
