# aen-power-domains

Bench RAM-run proof of the SoM power-domain runtime ([#2784](https://github.com/alplabai/alp-sdk/issues/2784),
unit U5) on the **E1M-AEN803** (Alif Ensemble E8, M55-HE). It runs the quiesce
-> hold -> restore cycle in RUN mode (no STOP, no wake source, no cold boot) and
checks that every chip answers again afterwards.

See [`docs/aen-power-domains.md`](../../../docs/aen-power-domains.md) for the model.

## What it does

1. Brings the CC3501E bridge up (the one `WIFI_EN` toggle in the run) and
   records a baseline: CC3501E PING, DP83825 PHY ID over MDIO, NOR JEDEC ID,
   TMP112 temperature.
2. Prints every domain through `alp_power_domain_info()` and checks that
   `alp_power_domain_policy_set(WIFI_BLE, RAIL_OFF)` is **refused** (the Kconfig
   gate `CONFIG_ALP_SDK_SOM_PD_WIFI_RAIL_OFF` is off).
3. Quiesces every domain, holds 5 s, and checks the held state: TMP112
   `CONFIG.SD` set, CC3501E PING down.
4. Restores every domain (reverse order) and re-checks each chip: CC3501E PING
   with **no `WIFI_EN` toggle** (nRESET release only), PHY ID and link, NOR JEDEC
   ID, TMP112 temperature.

## Build and run

```
ZEPHYR_BASE=<zephyr-base> west build \
  -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-power-domains -- \
  "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
```

RAM-run it through J-Link (Flow C); no flash is written. Console is the E1M edge
UART0 at 115200 8N1.

## Bench contract

One line per step, grep-stable:

```
POWER_DOMAINS: <step> <PASS|FAIL>
POWER_DOMAINS: SUMMARY pass=<n> fail=<n>
```

Steps, in order: `baseline_ping baseline_phy_id baseline_jedec baseline_temp
rail_off_refused quiesce held_temp_sd held_ping_down restore ping_after
phy_id_after phy_link_after jedec_after temp_after`.

## Notes

- Behaviour on silicon is a bench result; `native_sim` cannot carry it. The
  logic (policy, order, record, boot restore) is covered by
  `tests/unit/power_som_domains`.
- The example reaches the SDK-internal quiesce / restore pair
  (`src/backends/power/som_power.h`); the portable surface is only
  `alp_power_domain_policy_set()`, `alp_power_domain_info()` and
  `alp_power_boot_wake_info()`.
- Whether the LPGPIO outputs (P15_n) hold through STOP itself is not tested
  here; that is the STOP bench (U8).
