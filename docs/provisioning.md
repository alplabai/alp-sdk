# Provisioning an E1M SoM

`scripts/provision_som.py` provisions one module from a versioned SoM-release
bundle (see the bundle manifest schema `metadata/schemas/som-release-bundle-v1.schema.json`).

It runs a linear, stop-on-first-failure sequence:

1. **validate** the bundle (`check_som_bundle.py`)
2. **HiL spec check** — if `--hil-spec` (or a `--carrier`-derived path; a relative
   `--hil-spec` resolves against `--alp-sdk-root`, or the repo root, not the working
   directory) is given, the directory must exist. This is checked here, before any
   flashing or serial allocation below, so a mis-derived `--carrier` **fails the run**
   (exit 1) before it can burn a manufacturing serial or write to the board. With
   neither flag, no test is wanted and this step is a no-op.
3. **flash** `bl2`/`fip` to xSPI (`xspi_flashwriter`, Flash Writer SCIF) and the
   system image to eMMC (`yocto_wic`) — the image is skipped for a
   `bootloader-only:image-pending-hw` bundle
4. **EEPROM** — allocate a serial, build the 128-byte manifest (`program_eeprom.py`).
   The RIIC0 @0x50 write + read-back-verify is HW-gated (see below) — this step only
   plans it, even under `--execute`.
5. **power-on test** — runs `tests/hil/run_smoke.py` for the board (gated on the
   firmware's `alp_hw_info_read()` succeeding first) if a HiL spec was given in step 2;
   a no-op (reported as skipped) otherwise.
6. **record** the unit to the ledger (`som_ledger.py record`) — `--test-result` is
   three-valued, one value per outcome (#1305):

   | value | meaning |
   |---|---|
   | `pass` | `--execute`, a test was configured, it ran, it passed |
   | `fail` | `--execute`, a test was configured, it ran, it **failed** |
   | `pending-hw` | everything else: dry-run, no test configured, or a test skipped under `--execute` |

   `fail` and `pending-hw` are distinct on purpose: "verified bad" and "not yet
   verified" are different states, and before #1305 both wrote `pending-hw`, so a
   unit that failed its power-on test was indistinguishable in the ledger from one
   nobody ever tested. Note that a **dry run never records `fail`** — the spec is
   validated rather than run, so a validation failure on a workstation with no board
   attached is not a unit that failed its power-on test.

## Safety: dry-run by default

Provisioning rewrites the bootloader, wipes eMMC, and programs the EEPROM, so the
tool **dry-runs by default** — it prints exactly what each step would do and exits
without touching hardware. Pass `--execute` to perform the real operations (it also
sets each backend's `confirm` gate).

## Examples

Dry-run a real bundle, exercising the (private) ledger:

```bash
python scripts/provision_som.py \
    --bundle ../alp-sdk-internal/releases/E1M-V2N101/som-0.1.0 \
    --ledger-root ../alp-sdk-internal/ledger \
    --som-ledger ../alp-sdk-internal/scripts/som_ledger.py \
    --carrier x-evk --by lab
```

Real provisioning on the bench (HW-gated; needs the SCIF port + Flash Writer):

```bash
python scripts/provision_som.py --execute \
    --bundle <bundle> --port COM24 \
    --flash-writer <Flash_Writer_SCIF_RZV2N_DEV_LPDDR4X.mot> \
    --emmc-device /dev/sdX --hil-spec tests/hil/v2n101-x-evk \
    --ledger-root <ledger> --som-ledger <som_ledger.py> --station bench1 --by you
```

## HW-gated transports

The real xSPI Flash-Writer serial write, the eMMC transport (host `/dev/` vs U-Boot
over serial), and the EEPROM i2c write are validated on the bench; until then run
`--dry-run` (the default). The bench-specific operational detail (ports, PSU, J-Link)
lives in the internal `flashing-and-bench-debugging-v2n` skill.
