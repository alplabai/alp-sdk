### Added — `aen-evk-demo` phase 9 does a real SD-card write → read → verify round trip (#2035)

Phase 9 was a `SKIPPED` stub with two blockers, and both have now cleared: a
microSD card is fitted on the bench, and the mux-select jumper is fitted on
header **P18**. Ten of the demo's fourteen phases are now implemented and four
remain stubs (this supersedes the eight/six count in the phase-8 fragment for
this issue).

**The card is behind a mux, and only one of its two controls is software.**
The microSD sits behind a pair of 74LVC157s. The **ENABLE** is E1M `IO20` →
CC3501E `GPIO_26`, routed on *both* module revisions, so the phase drives it —
low, `/E` being active-low — through the portable `alp_gpio_*` API on
`ALP_E1M_GPIO_IO20`. The CC3501E GPIO proxy turns that into a bridge
transaction; the raw `GPIO_26` index lives in the app's route table, not in
app code. The **SELECT** is E1M `IO21` and is not drivable at all on this
module: on r1 it reached CC3501E `GPIO_30`, and on r2 `GPIO_30` moved to `IO8`
and `IO21` was left open, reaching neither chip. Its position is set by hand on
header P18 — a fitted jumper pulls `MUX_SEL.SDIO` high through `R198`, an open
header lets `R27` pull it to `0V` — and that net drives both mux select inputs
(`U38` pin 1 `S`, `U39` pin 1 `S`) as well as E2 `L3` = `IO21`.

**Contention warning, r1 boards only.** Because `MUX_SEL.SDIO` reaches both the
P18 header and E2 `IO21`, an r1 module driving `IO21` from firmware while a
jumper is fitted puts a driven pin against the header rail. Fit the jumper or
drive the pin, never both. On r2 the module end is open, so the header is the
only driver. The phase never drives `IO21` on any revision — there is no
revision where doing so is both useful and safe — and the route table omits it
deliberately.

**Phase 8 must have run, and this phase re-uses what it left.** The ENABLE
rides the CC3501E GPIO proxy, which only routes a pin once
`cc3501e_bridge_bringup()` has powered the coprocessor, reset it and called
`alp_gpio_cc3501e_attach()`. Phase 8 does that and deliberately does not undo
it, so phase 9 opens the pin against the live bridge rather than re-running the
~900 ms power/reset sequence. Worth knowing about the failure mode: with no
bridge attached the proxy **delegates** `IO20` to the platform GPIO driver
rather than refusing it, so a phase 8 that failed produces a clean-looking
write to an Alif pad that is not connected to the mux, and then a card that
never enumerates. The ENABLE result gets its own log line so that case is at
least visible.

**`PASS` is the round trip, and nothing weaker.** After the ENABLE the phase
runs `disk_access_init("SD")` on the vendored `snps,dwc-sdhc` controller (the
overlay adds `sdhc@48102000` and its SD "D" pad route — CLK `P4_1`, CMD `P4_2`,
D0..D3 `P6_0`..`P6_3`, with `input-enable` on the five bidirectional lines
because the SoC cannot sample the card's responses without it), prints the
geometry verbatim, mounts FAT, then **writes, reads back and `memcmp`s**. A
`disk_access_init` returning 0 is not a pass; neither is the geometry reading
back. That is the same claim as the chip-ID read this app exists to stop
counting, so geometry is printed as information and is never gated on.

**A card in the slot is someone's, so the write is fenced in twice.** The phase
touches exactly one file it owns, `/ALPDEMO.TXT`: no other file, no partition
table, and **no formatting, ever**. That is enforced at two layers, because
losing either one alone would silently destroy a stranger's card —
`FS_MOUNT_FLAG_NO_FORMAT` at the mount site, and `CONFIG_FS_FATFS_MOUNT_MKFS=n`
in `prj.conf`, which leaves the mkfs code out of the image entirely so there is
no format path to reach by mistake from this or any future phase. A card with
no filesystem is a `SKIPPED` with that reason; it is not an invitation to make
one. The file is unmounted on every exit path, including the failing ones,
because a mounted volume with a dirty FAT cache left behind is how a card gets
corrupted for the next person.

**Five outcomes, kept apart, because they send you to different places.**

* **mux ENABLE could not be driven** → `FAIL`. `IO20` is routed on both
  revisions and phase 8 leaves the bridge up, so nothing about this board
  justifies it failing. Points at the bridge or the route table, not the card.
* **no card detected** → `SKIPPED`. `disk_access_init` answers
  `DISK_STATUS_NOMEDIA`, straight from the controller's `PSTATE` `CARD_INSRT`
  bit (no `cd-gpios`, so the driver falls back to it). The mux sits between the
  card and that bit, so a wrong P18 jumper position lands here too — the log
  names both causes rather than letting a reader stop at "empty slot".
* **controller failed to init** → `FAIL`. Any other non-zero: the card is
  detected and the SD handshake still did not complete, so it is the
  controller, the pinmux or the clock ramp.
* **card present but no filesystem** → `SKIPPED`. `fs_mount` answers `-ENODEV`
  (`FR_NO_FILESYSTEM`, translated).
* **write or verify mismatch** → `FAIL`, with both buffers printed, because
  which bytes differ is the whole diagnostic.

**A stale file cannot be mistaken for a fresh write.** `/ALPDEMO.TXT` survives
the run, so the next run opens a file that already has a plausible payload in
it — and if the payload were fixed text, a write that silently did nothing
would read back byte-identical and pass. The payload therefore carries a
per-run nonce (`k_cycle_get_32()`, sampled after the bridge bring-up, a Wi-Fi
scan and a BLE enable have each spent varying wall-clock time), printed in the
log and compared against the buffer *this* run built in RAM. The file is also
`fs_truncate`d to the bytes just written, so a longer leftover cannot hide
stale tail bytes behind a matching prefix, and `fs_sync`ed before the read-back
so the verify cannot be served out of the FATFS cache — a "verify" against
data that never reached the card would be the same false pass in a new place.

**`west.yml` now allowlists the `fatfs` module, and it was silently missing.**
`CONFIG_FAT_FILESYSTEM_ELM` `depends on ZEPHYR_FATFS_MODULE`, which Zephyr's
module-resolution pass sets only once the module has actually been fetched.
Without the allowlist entry that dependency is unmet and **both**
`CONFIG_FILE_SYSTEM=y` and `CONFIG_FAT_FILESYSTEM_ELM=y` are dropped from the
solve with no warning and no error — measured on this app, whose `.config`
simply had neither symbol. Same shape, and same fix, as the `littlefs` and
`hal_ethos_u` entries already in that allowlist, and the same caveat applies: a
workspace initialised before this entry landed needs a `west update` in its own
topdir first.

**`prj.conf`'s phase-8 block no longer contradicts its own config.** It used to
record `CONFIG_ALP_SDK_GPIO_CC3501E_PROXY` as deliberately off, on the grounds
that phase 8 makes no proxied-GPIO call and the app carried no route table. The
first half is still true; the second stopped being true here. The proxy is now
on and the app carries `src/cc3501e_gpio_routes.c`, and the comment says why.

That route table is the one in the tree that is **hand-written** rather than
emitted by `scripts/gen_cc3501e_gpio_routes.py` — that generator discovers its
targets by looking for a `board.yaml` beside a proxy-enabling `prj.conf`, and
`aen-evk-demo` is a standalone Zephyr app with no `board.yaml`, so it is
correctly skipped. Rather than hand-maintain the full nine-pad map (the
triplication #1859 removed), the file declares only the single pad phase 9
drives, and `tests/scripts/test_aen_cc3501e_routes.py` pins that entry against
`metadata/e1m_modules/aen/from-cc3501e.tsv` — the same source the generator
resolves through — asserts it stays a subset of the full map, asserts `IO20 →
26` specifically, and asserts `IO21` never appears.

Builds clean for `alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` and still
fits the Flow C ITCM budget with the SDHC driver and the FAT filesystem in the
image. That budget is the reason phase 14 stays stubbed, so it was measured
before the phase was written rather than after.

Not verified on silicon yet: every verdict here needs a card in the slot and
the P18 jumper in a known position, so the README's phase-9 transcript is
labelled illustrative rather than captured. The nine previously implemented
phases are unchanged.
