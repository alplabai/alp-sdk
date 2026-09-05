# aen-eeprom-provision

**Write** the 128-byte Alp hardware-info manifest into the on-module **24C128
EEPROM** on the E1M-AEN (Alif Ensemble) SoM, then read it back and verify.
The write sibling of [`aen-eeprom-manifest`](../aen-eeprom-manifest), which reads
the same bytes.

This is the step that gives a module its identity: SKU, hardware revision, serial
and manufacturing date. Run it once per unit, on the line, before the module ships.

## Why this exists

Until this app, nothing in alp-sdk could put an identity on a module.
`scripts/program_eeprom.py` packs the 128 bytes but never touches hardware — its
own docstring says so — and `scripts/provision_som.py`'s EEPROM step only *plans*
the I²C write:

> the i2c write+verify is HW-gated — this step only plans it; `--execute` does not
> perform it (no backend exists to)

The documented fallback was a third-party USB-I²C adapter (Aardvark / MCP2221 /
FT232H class) driven by its own vendor software: an extra fixture, an extra tool,
and a step no CI can check.

But the module's own M55 already reaches the EEPROM over SoC I²C2 — the read
sibling proves it on every run. So the writer needs no fixture at all. It is this
app, RAM-run over SWD.

## The bus

Same as the reader: the EEPROM's interface is selected by **bridge/DNP resistors**
onto **SoC I2C2** — a Synopsys DesignWare master bus (pins `P5_6 SCL_C` /
`P5_7 SDA_C`), driven by **upstream Zephyr's `i2c_dw`**. It is **not** on BRD_I2C
(SoC I2C0, which carries the RTC/TMP112/OPTIGA instead). The board overlay
(`boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay`) enables `i2c2`,
supplies its pinctrl and aliases portable bus 0 (`alp-i2c0`) to it.

The 24C128 has 64-byte pages, so a 128-byte manifest is two page writes with an
internal write cycle between them. `eeprom_24c128_write()` handles the split and
the ACK-polling; nothing here needs to know about it. A bare 128-byte I²C write
would silently wrap inside one page and corrupt the manifest.

## Run it

Provisioning is a **RAM-run (Flow C)**. Nothing is written to MRAM, so the module
is left carrying whatever application the line intends — not this tool.

```sh
# 0. Hold the bench reservation, as for any bench operation.
export LG_COORDINATOR=100.64.0.1:20408
labgrid-client -p e1m-aen-evk-01 acquire

# 1. Pack this unit's identity on the host.  The serial must come from the
#    allocation ledger, not from your head.
python3 scripts/program_eeprom.py \
    --board-yaml examples/aen/aen-eeprom-provision/board.yaml \
    --serial 2026W36-0001 \
    --mfg-date 2026-09-04 \
    --output /tmp/manifest-2026W36-0001.bin

# 2. Build ITCM-linked, with that blob baked in.
B=scripts/bench/aen
bash $B/build.sh "$PWD/examples/aen/aen-eeprom-provision" \
    -DALP_MANIFEST_BIN=/tmp/manifest-2026W36-0001.bin \
    "-DEXTRA_CONF_FILE=$PWD/$B/aen-bench-shared.conf;$PWD/$B/aen-flowc-itcm.conf" \
    "-DEXTRA_DTC_OVERLAY_FILE=$PWD/$B/aen-flowc-itcm.overlay"

# 3. Write it.
bash $B/ram-run.sh "$PWD/build/aen-eeprom-provision"

labgrid-client -p e1m-aen-evk-01 release
```

Expected on a blank module:

```
[provision] to write: family=aen sku=E1M-AEN801 hw_rev=r2 serial=2026W36-0001 mfg=2026-09-04
[provision] wrote 128 bytes at offset 0
[provision] verified on device: family=aen sku=E1M-AEN801 hw_rev=r2 serial=2026W36-0001 mfg=2026-09-04
RESULT PASS: manifest written and verified on device
```

## Two refusals, both deliberate

**The build fails without `-DALP_MANIFEST_BIN`.** There is no default blob. A
placeholder identity written onto a module is indistinguishable from a real one
later, and the module would ship carrying a serial nobody allocated. A build error
is the cheap failure; a mis-identified module in the field is not.

**It will not overwrite an existing manifest.** A module that already carries a
valid manifest has a serial someone recorded; replacing it silently orphans that
record. Re-provisioning is legitimate — a mis-programmed unit, a schema
migration — but it must be deliberate:

```sh
... -DALP_PROVISION_FORCE=1
```

and the reason belongs in the unit's record.

## Verify independently

Trust the reader, not this app's own read-back — a different binary exercising a
different code path is the stronger check:

```sh
bash scripts/bench/aen/ram-run.sh "$PWD/build/aen-eeprom-manifest"
```

The strongest confirmation is the boot banner of *any* later build. Before
provisioning it falls back to the board name; after, it prints the real identity
read from the EEPROM:

```
Alp SDK 0.16.0  |  alp_e1m_aen801_m55_he  |  (c) Alp Lab AB     <- before
Alp SDK 0.16.0  |  E1M-AEN801 r2          |  (c) Alp Lab AB     <- after
```

## Order matters

Do this **before** anything that burns a fuse. The EEPROM is rewritable, so a
wrong manifest here is a re-run. The CC3501E's root-of-trust and MAC fuses are
one-time and OR-only — a mistake there is a scrapped part. Prove the module with
the reversible step first.

## Known gap: no `manufacturer` field

The schema (`include/alp/hw_info.h`) carries `family`, `sku`, `hw_rev`, `serial`
and the manufacturing date, but **no manufacturer**. Adding one means a
`schema_version` bump, and `alp_hw_info_classify_manifest()` currently rejects
`schema_version != 1` as `ALP_ERR_IO` — so a v2 module would read as a *corrupt*
EEPROM to any v1-built firmware, not as "newer". Any such change must also clear
`scripts/check_board_id_doc_parity.py`, which cross-checks the header,
`docs/board-id.md` and `scripts/program_eeprom.py`.
