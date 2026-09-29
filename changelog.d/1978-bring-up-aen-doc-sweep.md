### Documented — `docs/bring-up-aen.md` still called the TMP112's `0x40` address a per-unit defect after #1978 fixed the metadata (#1978)

`metadata/e1m_modules/E1M-AEN*.yaml`, the generated devicetree, `metadata/chips/tmp112.yaml`
and `docs/soms/aen.md` all already carry the corrected design address --
`0x40`, not the earlier-declared `0x48` -- for the on-module TMP112. The fix
that landed under #1978 (`fix(aen): repair five E1M-EVK drivers, correct the
TMP112 address, and add the phased EVK demo (#2036)`) did not touch
`docs/bring-up-aen.md`, which still called `0x40` a "per-unit TMP112 defect",
listed `0x48` as the "DECLARED address" that "does NOT answer on the 2026W36
batch", and reported the bench-settled BRD_I2C table's ACK for the TMP112 at
`0x48` instead of `0x40`. All three are corrected to match the rest of the
tree: `0x40` is the TMP112DIDPWR's (X2SON-5) design address per TI SBOS473L
Table 7-4 (ADD0->GND), not a batch anomaly.
