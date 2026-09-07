### Fixed — three I2C addresses the shipped docs, examples and headers still got wrong (#1974, #1976, #1978)

A bench sweep of the E1M-EVK and the on-module E1M-AEN803 contradicted three
addresses that were still asserted across public headers, metadata, examples and
docs. Each is corrected everywhere it appeared, not only where it was first
noticed.

- **The main TCAL9538 I/O expander (U35) is at `0x73`, not `0x72`** — 2 of 2
  boards ACK at `0x73` and are silent at `0x72` (alp-sdk#1974). The
  `EVK_I2C_ADDR_TCAL9538_MAIN` macro was corrected earlier; this sweep fixes the
  copies that had drifted from it, including a copy-paste snippet in
  `include/alp/boards/alp_e1m_evk.h` that told the reader to call
  `tcal9538_init(&io_exp, i2c_bus, 0x72)` — an address that answers on neither
  board. The snippet now uses the macro so it cannot desynchronise again. The
  second expander U37 (`0x71`) is marked NOT ASSEMBLED on this revision.
  `metadata/chips/tcal9538.yaml` gains the missing `0x73` strap row; its `0x72`
  row is kept, because `0x72` remains a legal strap for A1=1/A0=0.
- **`0x48` on this bus is the TAS2563 global/broadcast address**, not an
  unidentified device (alp-sdk#1976). `docs/tutorials/02-i2c-scan.md`'s
  expected-output table had no `0x48` row at all, so a customer running the
  documented scan saw an ACK the docs never predicted.
- **The on-module TMP112 answers at `0x40`, not its declared `0x48`, on 2 of 2
  modules** (alp-sdk#1978) — a batch property of the 2026W36 build, not the
  per-unit solder defect the example, its overlay and `docs/soms/aen.md` all
  described. That framing sent readers to inspect a board that has nothing wrong
  with it. The devicetree deliberately still declares `tmp112@48`: `0x40` is not
  a legal TMP112 strap address at all, so whatever answers there has not been
  shown to be the TMP112, and re-addressing the node would encode a guess. The
  consequence is now stated plainly — the stock `CONFIG_TMP112` driver does not
  bind on these modules.

`XEVK_I2C_ADDR_TCAL9538` (E1M-X-EVK) is deliberately left at `0x72`: that is a
different board and no measurement covers its strap. Changing it to match the
E1M-EVK would ship an unverified address.
