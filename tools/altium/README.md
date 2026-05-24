# Altium extraction tools

Scripts that pull **ground-truth** hardware data out of the Altium board
projects so the SDK's board/SoM metadata stays derived from the real
schematics rather than hand-transcribed. Re-run whenever a board revision
changes — this is the data source behind the "one machine-readable source
of truth" principle.

## `export_e1m_pinmap.pas`

A single reusable procedure — **`ExportAll`** — that works on any board
project (E1M-EVK, E1M-X-EVK, a SoM project, …). One run writes two CSVs:

| File | Columns |
|------|---------|
| `<base>_pinmap.csv` | `Net, Designator, PinNumber, PinName` |
| `<base>_components.csv` | `Designator, Comment, Parameters` |

The **pin map** is the key one: a normal netlist export gives
`net ↔ pin number` but drops the **pin name**, and for an E1M / E1M-X
module connector (e.g. designator `E2`) the pin *name* is the E1M-X pad
(`IO4`, `PWM3`, `ADC0`, `ENC0_X`, …). So it yields, in one file:

```
E1M-X pad  ↔  board net  ↔  on-board chip pin
```

The **components** dump captures every component parameter, so MPN /
Manufacturer values come straight from the schematic.

### Run it
1. Open the board project in Altium so it's the **focused** project.
2. **File → Run Script…** (older Altium: **DXP → Run Script…**).
3. Pick **`ExportAll`**, Run.
4. At the prompt, set the output base path — **rename per board**
   (e.g. `…\xevk`, `…\e1m_evk`, `…\v2n`). Default base:
   `C:\Users\caner\AppData\Local\Temp\board`.
5. Send both CSVs over.

Uses the Document-Manager API on the flattened document; builds output in
memory and writes with `SaveToFile` (no lingering file handle — the cause
of the earlier "I/O error 32"). The pin-map is written first, so even if
the component pass hits a differently-spelled `DM_*` member, the pin-map
CSV is already on disk.

### Output → SDK metadata
- `E2` rows of `_pinmap.csv` → the E1M-X **pad → net** map → fills
  `metadata/boards/e1m-x-evk.yaml` `e1m_routes:` and validates
  `include/alp/e1m_x_pinout.h`.
- `_components.csv` → the `populated:` chip list + MPNs.
- Run on the **E1M-EVK** and each **SoM** project the same way for their
  metadata / per-SoM `pad_routes:`.

### Notes
- Raw CSVs are **working data — not committed**. Only the sanitised,
  customer-buildable subset lands in `metadata/`.
- DM member names vary slightly by Altium build; if one is "Undeclared
  identifier", paste it — each is a one-line swap.
