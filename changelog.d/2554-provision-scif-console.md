### Fixed — V2N provisioning lost the one-shot ROM banner and mis-streamed the Flash Writer over SCIF (#2554)

Found on E1M-V2M103 2026W38-0003 bootstrap. Three fixes in `scripts/provision/`:

- **Console is read during the power-off window.** `Power.cycle()` now takes the console and pumps it into its buffer for the whole off time (`scripts/provision/bench.py` "Keep reading the console while off"), so a banner printed right at power-on is not lost; every cycle-then-expect site passes its console, including Detect.
- **LF-only `.mot` files load.** The boot ROM answers "Address Error!!!" to LF-terminated S-records, so `load_writer` normalises line endings to CRLF (`scripts/provision/scif_writer.py` "ROM rejects an LF-only S-record file").
- **Streaming waits for the ROM's full prompt line.** The `SCI Download mode` banner matches mid-line, and streaming then collided with the ROM's own output ("Invalid Character Error!!!"); `load_writer` now waits for `ROM_READY` ("Load Program to SRAM") plus 0.3 s.
