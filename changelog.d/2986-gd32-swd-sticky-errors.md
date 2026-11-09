### Fixed — gd32_swd clears sticky SWD and FMC errors (#2986)

**A FAULT now writes DP ABORT `0x1E`, an exhausted WAIT retry writes DAPABORT (`0x01`), and `FMC_SR` error bits (`0xF8`) are write-1-cleared before each erase and program.** Before, one FAULT left STICKYERR set and one stale FMC error bit failed every later flash operation until a power cycle.
