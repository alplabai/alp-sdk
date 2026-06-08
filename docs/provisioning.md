# Provisioning an E1M SoM

`scripts/provision_som.py` provisions one module from a versioned SoM-release
bundle (see the bundle manifest schema `metadata/schemas/som-release-bundle-v1.schema.json`).

It runs a linear, stop-on-first-failure sequence:

1. **validate** the bundle (`check_som_bundle.py`)
2. **flash** `bl2`/`fip` to xSPI (`xspi_flashwriter`, Flash Writer SCIF) and the
   system image to eMMC (`yocto_wic`) — the image is skipped for a
   `bootloader-only:image-pending-hw` bundle
3. **EEPROM** — allocate a serial, build the 128-byte manifest (`program_eeprom.py`),
   write it to RIIC0 @0x50 and **read it back to verify**
4. **power-on test** — `tests/hil/run_smoke.py` for the board (gated on the firmware's
   `alp_hw_info_read()` succeeding first)
5. **record** the unit to the ledger (`som_ledger.py record`)

## Safety: dry-run by default

Provisioning rewrites the bootloader, wipes eMMC, and programs the EEPROM, so the
tool **dry-runs by default** — it prints exactly what each step would do and exits
without touching hardware. Pass `--execute` to perform the real operations (it also
sets each backend's `confirm` gate).

## Examples

Dry-run a real bundle, exercising the (private) ledger:

```bash
python scripts/provision_som.py \
    --bundle ../alp-sdk-internal/releases/E1M-V2N101/som-1.0.0 \
    --ledger-root ../alp-sdk-internal/ledger \
    --som-ledger ../alp-sdk-internal/scripts/som_ledger.py \
    --carrier x-evk --by lab
```

Real provisioning on the bench (HW-gated; needs the SCIF port + Flash Writer):

```bash
python scripts/provision_som.py --execute \
    --bundle <bundle> --port COM24 \
    --flash-writer <Flash_Writer_SCIF_RZV2N_DEV_LPDDR4X.mot> \
    --emmc-device /dev/sdX --hil-spec tests/hil/e1m-v2n101-x-evk \
    --ledger-root <ledger> --som-ledger <som_ledger.py> --station bench1 --by you
```

## HW-gated transports

The real xSPI Flash-Writer serial write, the eMMC transport (host `/dev/` vs U-Boot
over serial), and the EEPROM i2c write are validated on the bench; until then run
`--dry-run` (the default). The bench-specific operational detail (ports, PSU, J-Link)
lives in the internal `flashing-and-bench-debugging-v2n` skill.
