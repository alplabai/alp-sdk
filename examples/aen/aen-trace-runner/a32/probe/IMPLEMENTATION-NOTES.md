# A32 probe -- implementation notes

Full detail behind the README; this is the record for anyone reviewing
`start.S` before it goes anywhere near hardware. Spec:
`docs/2026-09-22-a32-probe-spec.md`.

## Deviation from the spec's literal step order: park loop install moved earlier

Spec step 8 says "copy the park loop into SRAM0 at `0x023FE000` ... clean,
and branch there **at the end**" -- i.e. after MMU/cache bring-up (step 5-6)
and NEON enable (step 7). But step 7 explicitly anticipates a possible
`undef` on the `vmsr fpexc` write, and every fault handler's job is to
"record into the results block then park in SRAM" at `0x023FE000`. If the
park loop hadn't been copied there yet when that `undef` fires, the handler
would branch into whatever garbage SRAM held at cold boot -- undefined
behaviour, exactly the failure mode the fault handlers exist to avoid.

Fix: `start.S` copies the two-instruction park loop (`wfi` / `b` back to
the `wfi`) into `0x023FE000` immediately after `SP`/results-header setup,
**before** the cache/MMU/NEON sequence, not after. This is safe because
`0x023xxxxx` is identity-mapped (VA==PA) both with the MMU off (the copy is
a direct physical store, as it happens here) and once our own section table
is live (that table maps `0x023xxxxx` 1:1) -- so the address means the same
physical location either way, and every fault handler, however early it
fires, has a valid target.

Practical effect: `dcache_inv`/MMU-build/enable/NEON-enable is now the ONLY
window before `main()` where a synchronous fault can occur, and the park
loop is guaranteed to already exist for all of it. (Faults are still
possible in the handful of instructions between reset and the results-block
zero loop -- `VBAR`/`SCTLR.V` writes and `SP` load -- but those can't fault
in practice: no memory access, no coprocessor access gated by NS
permissions.)

## TTBR0 walk-attribute choice

The spec says only "`TTBR0 = table | (cacheable walk bits)`" without
specifying the exact bits. `TTB_BASE = 0x023E0000` is 16 KiB-aligned by
construction (it has to be, to hold the full 4096-entry table), which means
its low 14 bits -- exactly the bits that encode `S`/`RGN`/`IRGN` walk
attributes in `TTBR0` -- are all zero if we write `TTBR0 = TTB_BASE`
verbatim. All-zero decodes to **Non-cacheable, non-shareable** walks.

That is the deliberate choice here, not an oversight: the page table itself
lives in `0x02300000-0x023FFFFF`, which the same table maps as **Normal,
non-cacheable**. Giving the walk hardware a *cacheable* view of memory that
the data side declares *non-cacheable* is exactly the kind of two attributes
for one physical range mismatch the ARM ARM warns can produce incoherent
results. Since the table is built once, before the MMU is even enabled, and
never modified again, there is no performance case for caching the walk
that's worth that risk -- our TLB working set is 11 sections total (4
SRAM0 + 6 MRAM + defaults), so uncached walks cost nothing measurable.
Comment is inline in `start.S` at the `TTBR0` write.

## Cache/MMU enable sequence -- line-by-line self-review

Order actually executed (see `reset_handler` in `start.S`):

1. `VBAR` set, `SCTLR.V` cleared, `ISB` -- makes exception delivery land at
   our table before anything that can fault runs. `SCTLR` is "unprogrammed"
   per the boot contract, so `V`'s reset value can't be assumed; clearing it
   explicitly is required, not just defensive.
2. `SP = 0x023FF000` -- must happen before the first `str`/`bl` (results
   zeroing uses register-only addressing, but the fault-record path and
   `main()` both need a stack).
3. Results block zeroed (0x2C0 bytes), then `magic`/`stage=1` stamped.
4. Park loop copied to `0x023FE000`, `DSB` (store visible),
   `ICIALLU` (self-modifying-code rule: invalidate I-cache before the freshly
   written instructions can be fetched -- ARM ARM's guidance for code the
   core itself just wrote), `ISB` (pipeline sees the invalidate before any
   later fetch). This is the "moved earlier" park loop from above.
5. `ICIALLU`, `BPIALL`, `TLBIALL` -- three independent structures, no
   ordering dependency between them; grouped together and done once, with
   MMU/caches still off per the boot contract, so nothing can be
   speculatively refilled from stale state while we invalidate.
6. D-cache invalidate by set/way, all levels from `CLIDR` -- the canonical
   ARMv7-A/ARMv8-A(AArch32) sequence (`CLIDR`->level count,
   `CSSELR`+`ISB`->select level, `CCSIDR`->geometry, nested way/set loop,
   `DCISW` per set/way), transcribed with the reference's own register
   names so it can be diffed against the ARM ARM's published example
   line-for-line. `ISB` after the `CSSELR` write is required -- `CCSIDR`
   reads back the *newly selected* cache's geometry only after that ISB;
   without it the read can still see the previous level's geometry and the
   way/set shift amounts (`r5`/`r2`) come out wrong for every level after
   the first. Final `DSB` -- required so the invalidate has actually
   completed (per set/way maintenance is not guaranteed synchronous with
   the issuing instruction) before we start treating the caches as clean;
   `ISB` after so the pipeline doesn't have anything speculatively
   in flight from before the invalidate completed.
7. Section table build: fill all 4096 entries `Device` first (the memory
   plan's "everything else" default), then override the 4 SRAM0 entries
   and 6 MRAM entries the plan calls out by address. Pure stores to
   physical memory -- MMU is still off, so these can't fault and don't need
   cache maintenance (nothing has cached this memory yet; the D-cache
   invalidate above already guarantees that).
8. `TTBR0`/`TTBCR`/`DACR` written, then `DSB`+`ISB` -- `DSB` makes sure
   all 4096+10 table stores are in memory before the walk hardware can see
   them (table walks read memory directly, not through our CPU's own
   store-to-load forwarding); `ISB` before the enable in step 9 so the new
   `TTBR0`/`TTBCR`/`DACR` values are definitely the ones in effect for the
   very first translated access after `SCTLR.M` goes live.
9. `SCTLR |= M|C|I|Z`, `ISB` -- the `ISB` here is the architecturally
   required one: enabling the MMU/caches changes how every subsequent
   instruction fetch and data access is interpreted, and the ARM ARM
   requires context synchronization (`ISB`) immediately after such an
   `SCTLR` write before relying on the new behaviour. Read-modify-write
   (not a blind overwrite) so we don't clobber any `SCTLR` bit TF-A may
   have already set for a reason outside this spec's scope.
10. `CPACR` |= CP10/CP11 full access, `ISB`, then `vmsr fpexc, r0` -- the
    `ISB` between them is required because `CPACR` gates coprocessor
    access and that gate has to be visible to the pipeline before the
    `vmsr` (which targets exactly the coprocessor `CPACR` just unlocked) is
    decoded, or the `vmsr` could still trap even though the intent was to
    allow it. If NS access is refused anyway, `vmsr` undefs and
    `undef_handler` records `fault_code=1` with the real `DFSR`-family
    fields and `LR` -- this path is deliberately NOT probed/guessed around;
    the results block is the only source of truth for whether NEON came up.

Each `mcr`/`mrc`/`DCISW` line in `start.S` carries its own comment citing
which ARM ARM rule it implements (see the file directly); this section is
the summary pass, not a duplicate of those comments.

## Test methodology choices not pinned down by the spec

The spec fixes the test matrix and results layout exactly, but leaves pass
counts and the fill/clean split to the implementation:

- `PASSES_1MIB = 16`, `PASSES_16KIB = 512` for fill/read/copy -- chosen so
  each timed measurement moves double-digit MiB through the buffer, giving
  thousands of `CNTVCT` ticks (100 MHz) of resolution without a long run.
- Clean test: each of `PASSES_CLEAN_{1MIB,16KIB}` iterations does an
  **untimed** `neon_fill` (re-dirty the range) immediately followed by a
  **timed** `DCCMVAC` clean + `DSB`. Only the clean cost accumulates into
  the recorded ticks -- that is what "record clean ticks separately (this
  is the cost of making a cached back buffer visible to the CDC200)" asks
  for; timing fill+clean together would conflate the two.
- Cache line size for the `DCCMVAC` loop comes from `CTR.DminLine`
  (`4 << DminLine` bytes), read fresh inside `clean_dcache_range()` on every
  call rather than cached in a global -- there are no globals in this image
  at all (see below), and a fresh `mrc` is a handful of cycles, irrelevant
  next to the clean itself.

## No `.data`/`.bss`, verified

`link.ld` asserts `SIZEOF(.data) == 0` and `SIZEOF(.bss) == 0` at link time
(build fails otherwise), and `main.c` has zero global/static mutable
variables -- every piece of state (loop counters, the results pointer,
checksums) is a stack local, and the results block itself is only ever
reached through a pointer literal into SRAM, never a linker-owned symbol.
`objdump -h a32_probe.elf` confirms this after the build: the only section
present is `.text` (`CONTENTS, ALLOC, LOAD, READONLY, CODE`) -- `.data` and
`.bss` don't even appear, because the linker drops empty sections entirely.

## Verification performed (see the handback message for the actual output)

- `make` -- clean build, zero `-Wall -Wextra` warnings, `size` shows
  `data=0 bss=0`.
- `arm-none-eabi-objdump -d --start-address=0x80020000
  --stop-address=0x80020040` -- first 16 instructions are the 8-entry
  vector table (`b reset_handler` first) followed by the start of
  `reset_handler` (`VBAR`/`SCTLR` setup).
- `arm-none-eabi-objdump -h` -- single `.text` section, no writable
  section anywhere in the image.
- Full-image disassembly manually walked against the intended section-table
  values (found and fixed one real bug this way: the park loop's `b`
  encoding was originally `0xEAFFFFFE`, "branch to self", not
  `0xEAFFFFFD`, "branch back to the `wfi`" -- functionally still an
  infinite loop either way, but not the spec's `1: wfi; b 1b` idiom; fixed
  before this was committed).
- `python3 decode.py --selftest` -- builds a synthetic results block
  in-process (no hardware) and asserts the MB/s math against two
  hand-computed values (100 MB/s and 104857.6 MB/s).

## Review round 2 (coordinator review of c3fa4da) -- fixes applied

- **F1 (major): DCISW -> DCCISW.** The step-4 D-cache set/way pass ran with
  a plain invalidate (DCISW), which silently drops dirty lines with no
  writeback. This is a warm TF-A handoff, not a cold reset: sp_min (and
  whatever ran before it) may have executed with caches on and left live
  dirty secure-world state (its own RW data, live again on the next SMC
  such as PSCI CPU_ON) sitting in L1/L2. A bare invalidate over ALL
  sets/ways/levels would drop that with no writeback -- for our own
  not-yet-used regions harmless, for TF-A's memory, corruption. Changed to
  DCCISW (`c7,c14,2`, clean+invalidate) so dirty lines get written back
  before eviction; a clean of an already-clean or not-yet-used line is a
  same-value no-op, so this costs nothing for our own regions and fixes the
  real hazard for TF-A's.
- **F2: `dsb sy` before the park jump.** Added once, at the top of
  `goto_park` (both `record_fault` and the step-9 return path already
  funnel through that single label, so one insertion covers both call
  sites without repeating it) -- drains any store still in flight (the
  results-block write each caller just did, or NC-buffer test writes) so
  it's actually in memory, and visible over SWD, before the core parks.
- **F3: `read_cntvct()` hardening + write-drain before end-time.** Added
  `isb` before the `mrrc` (stops the counter read being spuriously
  reordered ahead of the work being timed) and a `"memory"` clobber (stops
  the compiler reordering the timed buffer accesses across the call).
  Added `dsb sy` before the `t1 = read_cntvct()` capture in the scalar-fill,
  neon-fill and neon-copy timed loops -- without it, a still-posted store
  wouldn't count against the measured interval, understating elapsed time
  and inflating the reported MB/s. (The clean test already had this for
  free: `clean_dcache_range()` ends with its own `dsb`.)
- **F4: checksum + sentinel.** The original uniform-constant fill
  (`0x5A5A5A5A` everywhere) made `neon_read_xor()`'s full-buffer XOR-reduce
  structurally 0 for both our word counts (262144 and 4096, both powers of
  2 >= 4): XOR-reducing any single repeated value, OR a plain arithmetic
  index `0,1,...,N-1`, cancels to 0 by construction (each output bit is
  toggled by exactly half the inputs) -- **for real data and for a
  silently-dropped write alike**. Verified this by direct computation
  before trusting it (see the Python check in the review-response commit
  message), not just by argument, because a plain-index pattern turns out
  to have the *same* blind spot for the same underlying reason. Fixed by
  multiplying the index by an odd multiplicative-hash constant
  (`FILL_MUL = 0x9E3779B1`, Knuth's) before XOR-ing in the base -- this
  breaks the cancellation for both our exact word counts, verified by
  direct computation, not just argued. Also found and fixed a second,
  independent instance of the same cancellation family: `neon_read_xor()`'s
  result was being re-XORed into `checksum` once per timed pass, and since
  the buffer never changes between passes, XOR-ing the same deterministic
  value `PASSES_1MIB`/`PASSES_16KIB` (16, 512 -- both even) times cancels
  to 0 regardless of the fill-pattern fix. Changed to sample the read-test
  result exactly once (the untimed warm pass's return value) instead of
  accumulating across passes, which was pure noise, not additional
  information, anyway. `decode.py` now replays the same fill-pattern math
  (`expected_checksum()`, literally re-running the same word-by-word loop
  main.c's NEON code computes, not a hand-derived closed form, specifically
  so it can't silently drift out of sync with main.c the way a closed-form
  re-derivation could) and flags a mismatch. Also added the explicit
  write-then-readback **sentinel** check per buffer region (`op=6`, beyond
  the base spec's op 1-5) -- write a known nonzero pattern, DSB, read it
  back, record pass/fail. This is the primary, unambiguous defence against
  a RAZ/WI firewall: no XOR-reduction math to get subtly wrong, just one
  word compared to what was just written, run before any throughput test
  touches that region. `decode.py` also now reports `ERROR (ticks=0)`
  instead of a misleading `0.00 MB/s`, and flags `CNTFRQ != 100000000`.
- **F5: SCTLR bits forced to 0, not left "unprogrammed".** `A` (alignment
  fault checking), `WXN`/`UWXN` (writable-implies-XN, which could silently
  XN our one deliberately-executable "writable-looking" 0x023xxxxx
  section), `EE` (exception endianness), `TRE` (TEX remap -- if left on,
  every TEX/C/B value in the whole page table means something different
  from what we computed it to mean) and `AFE` (Access Flag Enable -- changes
  what the AP field means; our AP=0b11 encoding assumes AFE=0) are now
  explicitly `bic`'d before the `M|C|I|Z` `orr`. `TRE`/`AFE` in particular
  were latent correctness bugs, not just hardening: if TF-A had happened to
  leave either set, the entire section-table attribute scheme built in step
  5 would have been silently misinterpreted by hardware.
- **F6: map only the one MRAM section in use.** The image is ~2.4 KiB,
  nowhere near a 1 MiB boundary; mapping all six MRAM megabytes the spec's
  memory plan allows for was unnecessary attack surface (a wild pointer bug
  could silently execute or cache-allocate against MRAM we never intended
  to touch). Now only `0x800` (`0x80000000-0x800FFFFF`) is mapped
  executable/cacheable; the rest falls through to the Device/XN default.
- **F7: comment fix.** "Device (strongly-ordered)" in the step-5 comment
  conflated two distinct VMSAv7 memory types -- our actual encoding
  (`TEX=000 C=0 B=1`) is Shareable Device, not Strongly-ordered
  (`TEX=000 C=0 B=0`). The code was already using the correct encoding
  (matches the spec's own table); only the prose was wrong. Corrected.
- **F8: faulting PC in decode.py.** Added `fault_pc()`, applying the ARM
  ARM's per-exception-type LR offset (Data Abort: `LR-8`; Undefined
  Instruction / Prefetch Abort: `LR-4`), printed alongside the existing
  `DFSR/DFAR/IFSR/IFAR/LR` line when a fault was recorded.

Rebuilt clean after all of the above: zero `-Wall -Wextra` warnings,
`a32_probe.bin` still XIP-only (single `.text` section, no `.data`/`.bss`).
`decode.py --selftest` now covers three cases: everything matches: checksum
OK, sentinel PASS, MB/s computed; the review-requested mismatch case:
checksum deliberately wrong (simulating a dead/firewalled region XOR-ing in
0 instead of the real per-test samples), sentinel FAIL, and a `ticks=0`
entry, all three flagged rather than silently reported as fine; and a
`CNTFRQ != 100 MHz` case, also flagged.

## Known limitation (accepted, not fixed)

A fault in the first handful of `reset_handler` instructions (`VBAR` write,
`SCTLR.V` clear, `SP` load) has no park loop to land on yet and no defined
recovery. None of those instructions touch memory or a
permission-gated coprocessor register, so this is not expected to be
reachable in practice; flagged here rather than engineered around.

## Stage 2 (silicon confirmed stage 1: magic OK, stage 0xD0E, no fault,
checksum OK, sentinels PASS, NC/WB/clean MB/s all sane, SRAM1 word read
returned without aborting)

Stage 2 adds: PMU clock measurement, PSCI CPU_ON of core1 + six dual-core
rounds (fill/clean/read on a new Inner-Shareable WB buffer, LDREX/STREX
stress, cross-L1 coherency), non-fatal CDC200 register reads, and SRAM1
(mailbox sentinel + core1's own park loop/heartbeat). Every new results
field is *appended* after the stage-1 layout (0x2C0 onward) -- nothing
stage-1 already reads moved.

### Non-fatal recoverable-fault mechanism (new: `probe_read32`, `pmu_try_read`)

The PMU and CDC200 accesses can legitimately fault (NS access blocked by
SDCR/MDCR, or a CDC200 firewall) and the task requires that NOT kill the
whole probe run. `record_fault` now branches on a new `probe_recovery_pc`
field: if a caller armed it (stored the address of its own resume label)
before a risky access, the handler skips the normal fatal record and
instead does a **proper exception return** -- overwrite the banked LR with
the resume address, then `movs pc, lr` (restores CPSR from SPSR *and*
branches, in one instruction). This is not optional ceremony: a plain
`ldr pc, =resume` would leave the core stuck in Abort/Undef mode with an
uninitialised banked SP_abt/SP_und, corrupting the very next `push`/`pop` in
the C code being "resumed" into. Single-core-at-a-time by construction
(only core0 ever arms it: the PMU test and CDC probe both run on core0,
sequentially, never while core1 is doing anything but idle-waiting on a
barrier) -- there is no cross-core race on the one shared
`probe_recovery_pc`/`probe_faulted` pair, but that's a real constraint on
any future caller, not just an implementation detail: a second concurrent
user would need its own pair.

`probe_read32`'s correctness detail worth flagging explicitly: on a fault,
the destination register of the aborted `ldr` is *unspecified* (ARM data
aborts don't complete the register writeback), so the fault path forces
the return value to 0 rather than trusting whatever was left in `r0`.

### PSCI CPU_ON / secondary_entry self-review

- **Target MPIDR**: derived at runtime from core0's own MPIDR, per the
  task's explicit instruction, not hardcoded. **Corrected in review round
  3 (item 4, MAJOR):** the original `bic Aff0(0xFF), orr Aff0=1` only
  cleared bits 7:0, leaving bit31 (RES1) and bits 30:24 (U/MT/reserved)
  copied straight through from core0's MPIDR into the PSCI `mpidr`
  argument -- the PSCI spec requires those bits to be 0. Alif's
  `ensemble_pwr_domain_on()` (`<Alif TF-A>/plat/alif/board/
  devkit_e7/common/devkit_e7_pm.c:17-36`) uses the value raw as an array
  index (`secondary_cpu_flags[cpu-1]`, `1<<cpu`) and only "worked" via
  32-bit index wraparound in that specific bl32 build -- not something to
  build on. Now masks with `0x00FFFF00` (keep Aff1/Aff2, clear everything
  else) before setting Aff0=1, giving exactly `0x00000001`.
- **Entry state assumption**: PSCI CPU_ON's entry-state contract for this
  platform isn't in our spec (only core0's primary-boot contract is, from
  TF-A source); `secondary_entry` assumes the standard PSCI/TF-A shape
  (ARM/SVC/NS/MMU-off/IRQ-FIQ-masked, same as core0's own documented
  entry) -- confirmed correct on review (round 3's "Checked fine" list).
  If it were wrong, the MMU-enable sequence would simply fault like
  anything else and be recorded (fatal path -- `probe_recovery_pc` is
  never armed on core1), not silently wrong.
- **No D-cache SET/WAY invalidate on core1** (deliberate, not an
  oversight) -- but the justification here was **wrong in review round 2
  and corrected in round 3 (item 12)**. The original text claimed "set/way
  maintenance is inherently cluster-wide", which is false: a set/way
  operation is issued per-core and only ever reaches the issuing core's
  own L1 (plus whatever L2 it shares with others) -- it does NOT reach
  into another core's L1. The actually-correct reason core1 doesn't need
  it: core1's own L1 has held nothing until this instruction (the core's
  own hardware reset sequence invalidates its L1/TLB before it fetches
  anything), so there is nothing dirty or stale in it to clean; the shared
  L2 (if any) was already cleaned by core0's set/way sweep before core1
  was ever powered on. From there, TF-A's `CPUECTLR.SMPEN` (set before
  releasing either core, per the ARMv7-A/ARMv8-A multiprocessing-
  extensions contract) keeps the two L1s coherent via the snoop unit --
  that is what makes "core0 cleaned it once, core1 trusts that" sound
  rather than a race. (ARM's own multi-core bring-up guidance also flags
  set/way from a second core as actively unsafe once concurrent access
  from the first is possible -- a second, independent reason not to redo
  it even if there WERE something to clean.) TLBIALL/ICIALLU/BPIALL ARE
  now issued on core1 (review item 11, added in round 3) -- cheap, and
  this core has never run this table through its own TLB or fetched
  through its own I-cache/BTB before, unlike the D-cache question above.
- **Deliberately NOT refactored into core0's path**: `secondary_entry`
  duplicates (rather than calls into) core0's TTBR0/TTBCR/DACR/SCTLR/
  CPACR/FPEXC sequence. Core0's own sequence is what actually ran on
  silicon for stage 1 -- refactoring it into a shared parameterised
  subroutine to avoid this ~20-line duplication would touch code already
  hardware-validated, for a benefit (DRY) that isn't worth that risk here.
  The duplication is between two SHORT, easily side-by-side-diffable
  blocks, not the long cache-invalidate/table-build sequence (which core1
  never repeats at all, so there's nothing to duplicate there).
- **`goto_park` now dispatches per-core** via `MPIDR.Aff0` (banked per-PE,
  always reflects whichever core executes the instruction) since the two
  cores park in different places (core0: SRAM0 `0x023FE000`; core1: SRAM1
  `0x02402000`). This is shared by both cores' normal-completion paths
  *and* both cores' fatal-fault paths -- a fault on either core, at any
  point, still correctly finds its own park loop.
- **Stacks**: core1's stack (`CORE1_STACK_TOP = 0x023D0000`) sits in the
  same SRAM0 `0x023xxxxx` section as core0's, well below `TTB_BASE`
  (`0x023E0000`) and nowhere near core0's stack (grows down from
  `0x023FF000`) or the page table. **Correction (review round 3, item
  12):** round 2's notes said "~850 KiB of headroom" -- that measured to
  the section base (`0x02300000`), which isn't the boundary that actually
  matters. The real headroom is to the TF-A secure MHU0 window
  (`0x02380000-0x02380FFF`, never to be touched): `0x023D0000 -
  0x02381000` = ~316 KiB. Still comfortably more than this probe's stack
  usage needs, just a smaller number than originally stated.

### Heartbeat park loop

Stage 1's 2-instruction hand-encoded park loop (`wfi`; `b` back) is now a
6-instruction-plus-data-word template (`heartbeat_park_template` in
`start.S`, assembled normally rather than hand-encoded like stage 1's
version -- safer once it's more than a couple of instructions) that
increments a heartbeat word each `wfi` cycle. It finds its own heartbeat
slot via `adr` (PC-relative), not an absolute address, specifically so the
IDENTICAL template can be copied to either core0's `PARK_LOOP_SRAM` or
core1's `CORE1_PARK_ADDR` and have each copy correctly reference *its own*
adjacent heartbeat word -- confirmed by disassembly (`add r0, pc, #12`
resolves to the correct offset regardless of load address). This is a
change to the code path that ran on real silicon for stage 1; the change
was unavoidable (heartbeats are an explicit requirement), so it got the
same disassembly-level line-by-line check as everything else new here
(see the "Verification performed" list below).

### A bug caught by working through the LDREX/STREX ordering by hand

First draft had core0 initialise the shared counter to 0 *after* already
signalling `dual_go[4]` to core1. That's a real race: core1's
`barrier_wait` could return (seeing `dual_go[4]==1`) and start
incrementing the counter *before* core0's `*counter = 0` store, meaning
core1 could add 1,000,000 to whatever garbage SRAM1 held at boot (SRAM1
is never zeroed the way the results block is). The final value would then
be wrong for a reason that has nothing to do with whether the exclusive
monitor actually arbitrates correctly across the two cores -- a bug in
the TEST reported as "hardware LDREX/STREX is broken." Fixed: core0 now
initialises the counter and only *then* calls `barrier_signal` (which
itself does `dsb` before writing the flag), so the flag's own
release/acquire pairing (`dsb` before signal, `dmb` after observing) also
covers the counter initialisation -- core1 cannot observe `dual_go[4]==1`
before the zero is visible.

### Timeout on core0's waits for core1 (new: `barrier_wait_timeout`)

If core1 dies fatally partway through its own MMU/NEON bring-up (an undef
on `vmsr fpexc` is explicitly possible, same as core0), it never sets
`secondary_ready`, and without a bound, core0's `barrier_wait` on that flag
would spin forever -- meaning core0 would *also* never reach `stage=0xD0E`
or park, and the whole probe would look wedged instead of "core1 had a
problem, core0 finished and reported it." Every wait core0 does on a
core1-owned flag (`secondary_ready`, and each round's `dual_done[n]`) now
has a ~1 s CNTVCT-measured timeout (`BARRIER_TIMEOUT_TICKS`); on timeout,
`run_dual_core_rounds` bails out of the remaining rounds and `main()`
proceeds straight to the mailbox sentinel / CDC probe / park.

**Correction (review round 3, item 3):** the paragraph above originally
also claimed core1's own waits (on `dual_go[n]`, core0-owned) could stay
unbounded because "core1 simply parks (eventually) if core0 never gets
there." That was wrong: if core0 bails out of `run_dual_core_rounds` after
round *k* (because ITS wait timed out), core0 never calls
`barrier_signal(&dual_go[k+1])` -- there is no "eventually" for core1's
plain `barrier_wait` on that flag, it waits forever, stuck inside
`secondary_main`, and never reaches the code in `secondary_entry` that
parks it. Fixed: every one of core1's waits (`dual_go[n]` in each round
function, and the two flag-value waits inside `coherency_round`) is now
also `barrier_wait_timeout`-bounded, returning 0 up through
`run_dual_core_rounds` on timeout so `secondary_main` returns promptly and
`secondary_entry` falls through to `goto_park` regardless of which side
gave up first.

### Coherency-round pattern reuses the FILL_MUL fix, verified for the new word count

The 64 KiB cross-L1 buffer is `COH_BUF_WORDS = 16384` words -- also a
power of 2 (`2^14`), so it has the exact same XOR-reduce-over-a-uniform-
or-plain-index-pattern blind spot the stage-1 review fix (F4) found and
fixed for the throughput tests' word counts (4096, 262144). Verified by
direct computation (not just assumed to generalise) that
`FILL_BASE ^ (i*FILL_MUL)` XOR-reduces to a nonzero, deterministic value
for 16384 too before relying on it:
`0xc5330000`. `decode.py` computes this the same way it already does for
`checksum` (literal replay of the same word-by-word formula, not a
hand-derived closed form).

### Verification performed

- `make`: clean build, zero `-Wall -Wextra` warnings, `size` shows
  `data=0 bss=0` (unchanged property, now with the `static const`
  `cdc_addrs[3]` populating a previously-empty `.rodata` section --
  confirmed still `READONLY`, no writable section appeared).
- Disassembled and hand-verified against the design, address by address:
  `goto_park`'s MPIDR-based dispatch (both literal pool targets:
  `0x023fe000` and `0x02402000`); `heartbeat_park_template`'s `adr`
  resolving correctly (`add r0, pc, #12` -> the heartbeat slot); the new
  page-table entries (`0x02401c02`, `0x02511c1e`, `0x02611c1e` --
  confirmed `ATTR_WB_S1`'s S bit doesn't collide with the section base
  field); `probe_read32`/`pmu_try_read`/`psci_cpu_on_secondary`/
  `atomic_increment_n`/`secondary_entry` line by line against the source.
- `python3 decode.py --selftest`: 5 cases -- all-good (checksum, sentinel,
  MB/s, PMU MHz, PSCI SUCCESS, dual-core aggregate, LDREX/STREX,
  coherency, CDC POS_STAT-moved, mailbox all correct), the existing
  stage-1 mismatch case, bad CNTFRQ, core1-never-up (distinct from a round
  failure), and LDREX+coherency both independently wrong.

### Known limitations (accepted, not fixed)

- The fatal `fault_code`/`DFSR`/etc. fields are shared across both cores
  (not per-core). If both cores somehow fault fatally at the same instant,
  the loser's record is silently overwritten. Acceptable for a stage-2
  probe: the two cores' fault-capable windows don't overlap in the
  expected control flow (core1's only fault-capable window is its own
  MMU/NEON bring-up, during which core0 is idle-waiting on
  `secondary_ready` and cannot fault), and a genuine simultaneous double
  fault is exactly the kind of "something is very wrong" case where
  attribution ambiguity is a secondary concern.
- Dual-core round timing (`ldrex_core0_ticks`/`ldrex_core1_ticks`, the
  per-round `tests[]` ticks) are each core's own local measurement, not a
  synchronized wall-clock span -- reasonable since `CNTVCT` is a single
  system counter both cores read (no per-core clock-domain skew to worry
  about), but the two cores' start points differ by however long
  `barrier_wait`'s `wfe` polling takes to notice the go flag, which is not
  bounded/measured.

## Review round 3 (review of f61005d) -- 13 findings, all applied

**1 BLOCKER -- the event stream didn't exist, so every WFE timeout was
dead code.** `wfe` only returns early via an event: an unmasked interrupt
(none -- CPSR.I/F are masked for this whole image), an explicit `sev`
(only ever sent by the other core, or now by `goto_park`), or the generic
timer's event stream -- which TF-A never programs for us. Without it, a
core in `wfe` with no pending event just sleeps until the other core
happens to `sev` it; `barrier_wait_timeout`'s own timeout check only runs
*after* `wfe` returns, so with no event source it could never fire on its
own. Concretely: if core1 faults (record_fault's park path had no `sev`)
or stalls anywhere after PSCI CPU_ON returns SUCCESS, core0 sleeps in
`wfe` forever inside MRAM code, stage stuck at whatever it last was.
Fixed: `CNTKCTL.EVNTEN|EVNTI=15` set on both cores (own copies -- CNTKCTL
is per-PE) right after each core's CNTFRQ-equivalent setup point, giving a
stream event roughly every 655 us at 100 MHz; `sev` added right after
`dsb sy` in `goto_park` so a core about to park always wakes anyone
WFE-waiting on a flag it was never going to set.

**2 MAJOR -- park loop used `wfi`, which never wakes with everything
masked.** Same masking as above means `wfi` in the park loop would never
return either, so heartbeats would read 0 forever -- exactly the "is it
parked or wedged" signal the addendum asked for, defeated at the source.
Changed to `wfe` (now has an event source from fix 1).

**3 MAJOR -- core1's waits were unbounded; a core0 bail-out could strand
it forever.** See the "Correction" note under "Timeout on core0's waits
for core1" above -- every one of core1's waits is now
`barrier_wait_timeout`-bounded too, propagated as a 0 return through
`run_dual_core_rounds` so `secondary_main` returns and `secondary_entry`
reaches `goto_park` regardless of which side gives up first.

**4 MAJOR -- PSCI target MPIDR wasn't masked to spec.** See the
"Target MPIDR" correction under "PSCI CPU_ON / secondary_entry
self-review" above.

**5 MAJOR -- CDC200 reads were the riskiest thing in the probe and ran in
the middle, with no progress marker and no async-abort detection.**
CPSR.A is masked for this whole image and NS can't unmask it (SCR.AW=0),
so an asynchronous/external abort from an unclocked or unpowered
0x49031000 would never reach `data_abort_handler` -- it would either hang
the load outright or leave a garbage value with the synchronous fault
flag reading 0 (indistinguishable from success). Fixed: `cdc_probe` is now
the LAST thing `main()` does (mailbox sentinel moved before it, so
everything else is on record first); `r->stage = 0xCDC0+i` before each
individual read; `ISR.A` (`mrc p15,0,rX,c12,c1,0` bit 8) checked
immediately after every read and folded into `cdc_fault{0,1}` as bit1
(bit0 stays the synchronous-fault flag) -- decode.py's `CDC_FAULT_NAMES`
reports `sync-fault` / `async-pending` / `sync+async` distinctly.

**6 MAJOR -- the coherency test could pass falsely.** See the full
redesign in `coherency_round`'s own comment (main.c) -- summary: the flag
is now cleared before `dual_go[5]` (so a stale flag surviving a warm reset
on SRAM1, which is never zeroed, can't skip the handshake), core1 primes
its own L1 with the OLD content before core0 publishes (so the test
actually exercises cross-L1 invalidation, not just "can core1 read this
region at all"), and the published pattern is seeded from CNTVCT each run
(`coh_seed`, recorded so decode.py can recompute the same expected
checksum) so even a hypothetical stale-data false pass becomes a checksum
MISMATCH instead of a silent match.

**7 minor -- VBAR stayed at MRAM forever, including while parked.**
`install_heartbeat_park` now also writes an 8-word `b .` safe vector table
at `destination+0x40` (32-byte aligned) in the same park page; `goto_park`
points VBAR there (+`isb`) just before the final jump, for both cores. An
exception after park now finds its vector in SRAM, not in MRAM that a
later Flow D write might be tearing.

**8 minor -- the recoverable-fault path wasn't guarded per-core.**
`record_fault` now also checks `MPIDR.Aff0 == 0` before taking the
recoverable path -- `probe_recovery_pc` is only ever armed by core0, so a
fault on core1 during the (narrow, but real) window where core0 happens to
have it armed no longer gets treated as "expected".

**9 minor -- PMCCFILTR left at reset value.** `pmu_try_read` now writes
`PMCCFILTR = 0` explicitly (count cycles unconditionally, no P/U/NS/S
filtering) rather than trusting reset state. Confirmed harmless either way
on this path (TF-A leaves `SDCR.SPME=0`/`PMCR.D=0`, no AArch32 monitor
trap here) but not worth relying on that.

**10 minor -- 0x027xxxxx was "Device, just never touched", not actually
unmapped.** The fill loop's default (Device, RW) meant that region was
readable/writable, just something our own code chose not to reference --
"we didn't", not "it can't". Now stores an explicit translation-fault
descriptor (0) at that table entry: a real fault on any access, not a
policy.

**11 minor -- core1 skipped ALL cache/TLB maintenance, not just set/way.**
Added `ICIALLU`+`BPIALL`+`TLBIALL`+`dsb`+`isb` before core1's MMU enable --
cheap, and this core has never run anything through its own TLB or
I-cache/BTB before. The D-cache set/way skip remains deliberate (see the
corrected justification above); this is a different, narrower addition.

**12 nit -- two comments in round 2's own notes were wrong**, corrected in
place above: "set/way is inherently cluster-wide" (false -- see the
corrected PSCI/secondary_entry bullet) and "~850 KiB" of core1 stack
headroom (measured to the wrong boundary -- see the corrected Stacks
bullet).

**13 nit -- two CNTVCT waits used `while (v < target)` instead of a
subtraction.** Fixed in `pmu_clock_test` and `cdc_probe`'s ~1 ms wait to
`(now - start) < N`, matching the pattern `barrier_wait_timeout` already
used -- correct by construction across a counter wrap, not just in the
practically-always-true common case.

Rebuilt clean after all of the above: zero `-Wall -Wextra` warnings,
still a single `.text` + `.rodata` image (no writable section).
`decode.py --selftest` gained 4 more cases (case 6: a different
`coh_seed` changes the expected checksum, proving it's load-bearing;
case 7: the new CDC async-pending fault kind reports distinctly from a
sync fault; case 8: a stage-marker value resolves to its name in the
report) alongside the existing 5, all pass.
