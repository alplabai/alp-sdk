# E1M-AEN module pinout

Pin-to-function mapping for the E1M-AEN family of SoMs
(`E1M-AEN301` / `401` / `501` / `601` / `701` / `801` -- the
Alif-Ensemble-based E1M modules).

## Files

| File                       | Schema                                                              |
|----------------------------|---------------------------------------------------------------------|
| `from-alif.tsv`            | `e1m_pad \t e1m_function \t alif_peripheral \t alif_pad`            |
| `from-cc3501e.tsv`         | `e1m_pad \t e1m_function \t cc3501e_function \t cc3501e_pad`        |
| `inter-chip.tsv`           | `signal \t cc3501e_role \t cc3501e_pad \t alif_role \t alif_pad`    |
| `alif-ospi.tsv`            | `ospi_signal \t alif_pad`                                           |
| `alif-ethernet-phy.tsv`    | `phy_signal \t alif_pad`                                            |
| `hw-revisions.yaml`        | Per-rev SDK-version compatibility window                            |

## Two silicon sources

The E1M-AEN family carries the **Alif Ensemble** SoC plus the
on-module **TI CC3501E** Wi-Fi 6 + BLE 5.4 combo module.  E1M-edge
peripherals split between the two: `from-alif.tsv` covers everything
the Alif SoC owns directly; `from-cc3501e.tsv` covers the pads the
CC3501E owns (notably SPI1 + IO11 / IO13 / IO15..IO21).

Apps targeting CC3501E-owned pads dispatch through the
[`<alp/chips/cc3501e.h>`](../../../include/alp/chips/cc3501e.h)
host-control surface.
