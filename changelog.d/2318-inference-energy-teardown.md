### Fixed — aen-inference-energy no longer faults after printing its RESULT (#2318)

The example's TFLM interpreter was a local in `main()`, so every `return`
ran its destructor, which re-walks the model flatbuffer staged in SRAM0.
SRAM0 is shared by every core: run on the M55-HP with another image
resident on the M55-HE, the HE overwrote the staged model (a read-back of
`model_sram` at `0x02000000` after the run held a pixel pattern, not the
model), and the teardown faulted in
`flatbuffers::Table::GetOptionalFieldOffset` (PC `0x0001764a`). The
resolver and interpreter now live in never-destroyed static storage, so
no exit path walks the model again.

Bench-verified on E1M-AEN803 2026W36-0009 (`e1m-aen-evk-01`, Flow C
RAM-run landing on the M55-HP): unfixed 3 of 4 runs faulted after
`RESULT FAIL: no INA236 answered on EVK_I2C_BUS_SENSORS` (two USAGE
FAULTs, one BUS FAULT at BFAR `0xd795bcba`); fixed 0 of 6, each ending
idle with IPSR 0 and CFSR `0x00000000`.
