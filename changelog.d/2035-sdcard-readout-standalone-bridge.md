### Added — `aen-sdcard-readout` brings its own CC3501E bridge up and drives the SDIO mux, so it no longer needs `aen-evk-demo` (#2035)

`aen-sdcard-readout` was 72 lines with no GPIO code, no bridge bring-up and no
`alp_*` call: on the EVK the microSD sits behind a 74LVC157 mux whose ENABLE
(E1M `IO20` → CC3501E `GPIO_26`, both module revisions) is only drivable over
the CC3501E's inter-chip bridge, so a standalone run of this app measured a
disk that was electrically disconnected and reported it indistinguishably from
a real card fault — every SD bench load had to run `aen-evk-demo`'s other
thirteen phases just to reach the mux.

`main()` now runs `cc3501e_bridge_bringup()` (copied verbatim from
`aen-evk-demo`'s `src/cc3501e_bridge.{c,h}`, the silicon-proven template every
AEN app copies rather than reaches across into) and, only if that comes up
clean, opens `ALP_E1M_GPIO_IO20` through the portable `alp_gpio_*` API,
configures it as an output and drives it **LOW** — `/E` is active-low — via
the CC3501E GPIO proxy (`src/cc3501e_gpio_routes.c`, the app's one-pad route
table, same shape as `aen-evk-demo`'s). Both return codes are printed on their
own lines before `disk_access_init` ever runs, so a card fault is now
distinguishable from a bridge or mux fault instead of being silently folded
into "no card". A bridge or mux failure is now its own `RESULT FAIL` and stops
before `disk_access_init` — a disk error measured with the mux undriven is
worthless as a measurement, and running the DWC SDHC probe against an
unreachable card is exactly what cost a bench session before this fix.

**The mux stays asserted on exit, on the maintainer's explicit instruction.**
`/E` LOW is this board's working state, not a transient this app borrows and
restores — the same decision `aen-evk-demo` made in `79cdeca6d`, carried here
with the same reasoning: it also lets the pin be metered at `U38` pin 15 /
`U39` pin 15 at any time after this app runs, not just inside the settle
window. `alp_gpio_close()` only frees the host-side proxy handle; the CC3501E
keeps driving `GPIO_26` low regardless.

**Still read-only by construction.** No `CONFIG_FILE_SYSTEM`, no `fs_*` call,
no `disk_access_write`, no `mkfs` anywhere in this app — `disk_access_init`
plus the geometry ioctls, unchanged from before this fix. The card in the slot
belongs to the maintainer.

`prj.conf` gains `CONFIG_SPI`, `CONFIG_ALP_SDK_CHIP_CC3501E` and
`CONFIG_ALP_SDK_GPIO_CC3501E_PROXY` (mirroring `aen-evk-demo`'s block, not a
new variant), and the board overlay gains the SoM-internal SPI1 + LP-GPIO +
`alp,pin-array` nodes the bridge needs, transcribed from the same source.
Verified against the built `.config`, not just the Kconfig source: every added
symbol lands, including the two `depends on SPI && GPIO` ones that are easy to
silently lose (`CONFIG_SDHC_LOG_LEVEL_DBG` was lost exactly this way on a
different app the same day this landed, with no warning).

Builds clean for `alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`, and the
linked ELF was checked for the bridge bring-up call and the route table
symbol, not just a clean link — an app that links clean can still dead-strip
the one thing being verified.

**Not yours to chase further:** with the bridge and mux both proven driven,
the card still does not enumerate, for a distinct and already-diagnosed
reason under separate investigation — `SW_RST_CMD` never self-clears on this
controller. That is now the honest remaining gap for this app to help debug,
in place of the electrically-disconnected one it used to measure instead.

Also fixed in `aen-evk-demo/src/main.c`'s phase 9 (same issue): the
`DISK_STATUS_NOMEDIA`-adjacent failure message still named the retired SD "D"
pad route (`CLK P4_1 / CMD P4_2 / D0..D3 P6_0..P6_3`) after `03126705b` moved
the overlay to the "B" route (`CLK P14_1 / CMD P14_0 / D0..D3 P13_0..P13_3`),
so the app was misreporting its own pinout in exactly the log line meant to
help the next person debug a handshake failure.
