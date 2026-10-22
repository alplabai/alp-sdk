### Added — E1M EVK 2641-R3 on-board RP2040 debug probe docs (Refs #405)

- `docs/aen-rp2040-debug-probe.md`: the RP2040 (`U55`) CMSIS-DAP probe as the primary AEN debugger with J-Link optional, the USB identity and dual-CDC channel roles (VID/PID TBD, `alplabai/alp-studio#52`), and the open flash-backend/`tan` work. Not bench-verified; OpenOCD/pyOCD steps are TBD. Linked from `debugging-aen.md`, `bring-up-aen.md`, `soms/aen.md` and the docs index.
