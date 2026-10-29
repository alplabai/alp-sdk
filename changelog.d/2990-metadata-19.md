### Fixed — ci: join related pad tables in check_pin_conflicts (#2990)

The gate checked each TSV alone, so a pad claimed by two different AEN
Alif tables (from-alif, inter-chip, alif-ospi, alif-ethernet-phy) or by
the V2N base map and v2n-m1/m1-additions passed. Join those tables and
report cross-table claims. The six documented SD/CC3501E-SDIO mux pads
(P13_0..P13_3, P14_0, P14_1) are allowlisted with a pointer to
docs/cc3501e-bridge.md. Adds tests with injected cross-table conflicts.
