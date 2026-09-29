### Fixed - `aen-evk-demo`'s README described a stale I/O-expander phase and a stale sample transcript (#2035)

Five places had drifted from the code:

- Phase 4's row still said the I/O expander phase was "read-only" and did
  "configuration (0x03) and input port (0x00) register reads" -- since
  `5d007e450` the phase **writes** the polarity-inversion register (0x02)
  twice as part of a real round trip. Left stale, this was a
  safety-relevant claim about a chip whose `P0`-`P3` drive
  `LCD_PWR_EN`/`LCD_RST`/`CAM_EN`/`CTP_RST`.
- Phase 2's and phase 3's rows didn't mention the presence probe
  `3cb2a95a3` added, or that `ABSENT` no longer fails either phase --
  the verdict semantics changed and the table didn't say so.
- The scope paragraph said "ten... the remaining four are stubs"; counted
  against `PHASES[]` in `src/main.c` it is now eleven implemented and
  three stubs.
- The captured sample-output block still showed
  `phase  4/14: I/O expander answers (TCAL9538, read-only)`, a phase name
  and a register-read description the code no longer emits. It is now
  explicitly marked ILLUSTRATIVE (hand-built from `PHASES[]`'s real names
  and the real `printf` format string, not a captured run) rather than
  presented as something it never was.
