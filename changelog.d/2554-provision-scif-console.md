### Fixed — V2N provisioning lost the one-shot ROM banner and mis-streamed the Flash Writer over SCIF (#2554)

Found on E1M-V2M103 2026W38-0003 bootstrap. Three fixes in `scripts/provision/`:

- **Console is read during the power-off window.** `Power.cycle()` now takes the console and pumps it into its buffer for the whole off time (`scripts/provision/bench.py:308` ("Keep reading the console while off")), so a banner printed right at power-on is not lost; every cycle-then-expect site passes its console, including Detect.
- **LF-only `.mot` files load.** The boot ROM answers "Address Error!!!" to LF-terminated S-records, so `load_writer` normalises line endings to CRLF (`scripts/provision/scif_writer.py:97` ("the ROM rejects an LF-only S-record file")).
- **Streaming waits for the ROM's full prompt line.** The `SCI Download mode` banner matches mid-line, and streaming then collided with the ROM's own output ("Invalid Character Error!!!"); `load_writer` now waits for `ROM_READY` ("Load Program to SRAM") plus 0.3 s.

Further changes in the same run, so the next unit goes through hands-off:

- **GD32 flash works with no network.** (Files are decoded on the board with python3: its image has no `base64` binary.) A blank GD32 leaves both gbeth ports dead, so there was no SSH to flash it over. `gd32_flash` now runs right after `boot_sd_linux` and, when the unit is unreachable, pushes the SWD tools and the three images over the console shell (base64 in chunks under the tty line limit, md5-checked on the board, abort on mismatch), flashes, then cold-cycles and re-checks the IP (`scripts/provision/console_target.py:13` ("Files travel as base64")). SSH and the bench's own probe wrapper stay preferred. `boot_sd_linux` records "no network yet" instead of failing.
- **`census` finds the real port names.** The gbeth ports are `end0`/`end1`, not `eth0`/`eth1`; the ledger keys stay `eth0_*`/`eth1_*` by port index, and carrier, speed and the link-partner advertisement are folded into the `eth*_link` value (`scripts/provision/linux_target.py:1025` ("the Renesas gbeth ports are end0/end1")).
- **`cold_boot_test` retries a latched PHY once.** An `end0` without carrier (#2582) gets one extra cold cycle; the boot fails if `end0` is still down, whatever IP is present. The retry count is noted as `end0_no_carrier_retries` in the step evidence (it is not a ledger catalogue key).
- **SSH, scp and probe children no longer read stdin**, so a piped operator answer reaches the prompt instead of being eaten by a child (`scripts/provision/linux_target.py:134` ("stdin=subprocess.DEVNULL")).
- **`power.off_s` defaults to 15 s** when `bench.yaml` omits it (`scripts/provision/bench.py:717` ("DEFAULT_OFF_S = 15.0")).
