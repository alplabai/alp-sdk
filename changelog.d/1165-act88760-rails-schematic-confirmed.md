### Changed — all 13 ACT88760 rail roles in the V2N power tree are now confirmed against the module schematic (#1165)

The 13 rail roles themselves were filled by the guarded PMIC work (#2483); one
mapping, LDO3 to `VDD_eMMC_3V3`, had been made by elimination and carried a
"confirm against the schematic" note. That is now checked: the LDO3 output
feeds `VDD_eMMC_3V3`, and the seven buck outputs and the other five LDO/load-switch
outputs match the existing `metadata/e1m_modules/v2n/power-tree.yaml` rows
(including the Buck5/Buck6 numbering correction). Only the `evidence:` text
changes; no voltage, window or control value moves.
