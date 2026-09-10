### Changed — the CC3501E bridge's RX sample delay is now silicon-swept, and three comments about it corrected (#2039)

`CC3501E_BRIDGE_RX_SAMPLE_DLY` moves from **6** to **4**. Six worked, but it
sat only two steps — 10 ns — below a hard failure nobody had measured.

The sweep that found it ran on `E1M-AEN803` serial `2026W36-0002`, SPI1 at
25 MHz (`BAUDR 0x00000008` against `clock-frequency = <200000000>`, verified),
with `aen-evk-demo` phase 8 as the workload. `N` was swept `0..10` and each
value read back from `0x481040F0` to confirm it had actually taken:

* `N = 0..7` — pass, two or three runs each: bring-up `0`, `PING` on attempt 1
  of 25, byte-exact MAC, scan `0`, BLE `0`.
* `N = 8` — **fail, three runs of three.** Bring-up still returns `0`, but the
  link never answers: `PING` returns `-5` after all 25 attempts.
* `N = 9` — pass, two runs of two.
* `N = 10` — pass, three runs of three, except that one run returned MAC
  `46:3e:8a:10:b6:a7` — a single bit flipped in byte 0.

`4` is the centre of the measured-good `0..7` span: four steps (20 ns, half a
bit period at 25 MHz) clear of the failure, with no cliff on the low side at
all, since `0` is itself clean. The `8`-fails-but-`9`-and-`10`-pass shape is
recorded and deliberately **not** smoothed into a boundary — the sweep did not
explain it, so everything above `7` is documented as unqualified rather than as
graded margin.

**Three claims in the tree were contradicted and are now corrected rather than
quietly dropped.** `examples/aen/aen-cc3501e-bringup`'s overlay said `rx-delay =
<0>` "fails outright" and that `4` "fails too"; both passed three runs of three.
`cc3501e_bridge.h` claimed a clean `4..8` window; `8` hard-fails here — though
that claim was taken on `e1m-aen-evk-01` at roughly half the clock, so it is
re-scoped to the board and rate it was measured on rather than called wrong.

**What `rx-delay` in devicetree actually does is now written down where someone
will trip over it: nothing, after init.** `spi_dw_alif.c` writes
`RX_SAMPLE_DLY` at init and at PM resume only, `spi_dw_configure` never touches
`0xf0`, and `cc3501e_bridge.c` pokes the constant over it immediately after
`alp_spi_open()`. There is one exception, and it is a trap: the poke is wrapped
in `#if CC3501E_BRIDGE_RX_SAMPLE_DLY > 0`, so at `0` the devicetree value
survives untouched — which is why a naive `N=0` sweep point on the two examples
that declare `rx-delay = <2>` silently measures `2` instead. Both the poke and
both overlays now say so.

The constant lives in `cc3501e_bridge.h`, which is byte-identical across the
**seven** examples that carry the bring-up template — `aen-cc3501e-bringup`,
`-ble-gatt`, `-companion-tour`, `-gatt-register`, `-gpio`, `aen-evk-demo` and
`aen-usb-firstlight` — so all seven move together and the property holds.
`examples/peripheral-io/alp-console` keeps its own divergent copy: it runs at
1 MHz and performs no poke at all, so it was never a member of that set.
