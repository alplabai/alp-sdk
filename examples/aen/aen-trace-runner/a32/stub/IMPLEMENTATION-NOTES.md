# A32 resident stub -- implementation notes (T-A2)

Plan: `docs/superpowers/plans/2026-09-22-a32-renderer.md` sections 3-6, 8.
Mailbox: `src/ipc/tr_mbox.h` (authoritative: ctrl at +0x180, fault at +0x1C0).
Payload contract: the header comment of `a32/common/stub_abi.h` (the one
copy; payloads include it). Lineage: `a32/probe/` (silicon-proven), left
untouched.

## Files

| file | what |
|---|---|
| `a32/common/stub_abi.h` | addresses, re-entry points, release header, cmd/state/fault codes, payload contract |
| `a32/common/crc32.{c,h}` | zlib CRC32, pure C, built into the stub and the host tests |
| `a32/stub/start.S` | reset + relocation, vectors, `cpu_setup`, `mmu_on`, `cache_sync_all`, `payload_jump`, `psci_cpu_on`, re-entry |
| `a32/stub/stub.c` | table, mailbox init, CPU_ON, park loops, LAUNCH, re-entry, fault record, release mode |
| `a32/stub/mkpayload.py` | ctrl_entry/len/crc, J-Link LAUNCH/HALT command files, `release` packer, `--selftest` |
| `a32/stub/decode.py` | mailbox dump (savebin or `mem32` text), `--selftest` |
| `a32/common/payload_start.S`, `payload.ld`, `libc_min.c` | shared payload entry (core 1 -> its park, core 0 -> `payload_main`), flat link at 0x02500000 with `.bss`/stack outside the CRC'd image, memcpy/memset |
| `a32/payload-colorbar/` | CP-A2 test payload: 8 bars into TR_FB_A and TR_FB_B (MHU0 window skipped) |
| `a32/payload-echo/` | CP-A4 payload: `tr_mbox_take_in` -> NEON solid fill of in_fb -> `tr_mbox_publish_out` |
| `tests/host/test_crc32.c` | C CRC vs zlib values (runs in `tests/host/runner.sh`) |

Build: `make -C a32/stub` (`a32_stub.bin`, 3,264 B), `make -C a32/payload-colorbar`
(`colorbar.bin`, 424 B, CRC 0x03075E84, writes `colorbar-launch.jlink` / `halt.jlink`),
`make -C a32/stub test`. arm-none-eabi-gcc 13.3.1, probe flags +
`-ffp-contract=off`; the stub adds `-mno-unaligned-access` (table built with
the MMU off, where an unaligned access faults) and
`-fno-tree-loop-distribute-patterns` (no libc memset/memcpy to call). Zero warnings.

## Dev loop (plan sec 5)

1. Stub resident in A32_APP (one authorised Flow D, A32 parked). Cold cycle.
2. `mem32 0x02401000, 128` -> `decode.py`: magic OK, `stub_state=PARKED`,
   `core1=PARKED`, both heartbeats advancing between two dumps.
3. Run `colorbar-launch.jlink` (standalone: `si SWD`, `speed`, `device`
   from `JLINK_OPTS="--jlink-device NAME --jlink-speed KHZ"` at make time,
   default `Cortex-M55` / 4000 -- set it to the M55 AP profile the bench uses;
   probe serial stays on the JLinkExe command line; the image goes in with
   `loadbin ..., noreset` and nothing resets or halts a core). The file writes
   HALT first, then 5 x (`sleep 250`, `mem32 stub_state`) -- J-Link command
   files cannot branch, so this is a bounded wait that LOGS the state: the
   last read must be 0 (PARKED) or 2 (FAULT); 1 means a payload ignored HALT
   and the load that follows overwrote live code -- reset the A32. Then
   image, entry, len, crc, `ctrl_cmd=1` last.
4. Read: `stub_state=RUNNING`, `out_seq` +1, `out_fb=0x02000000`,
   `out_heartbeat` advancing, `core1=PARKED` (colour-bar core 1 returns at
   once), and the colour-bar record (`decode.py` prints it): progress 3,
   the three MHU0-window words (read only), ISR.A. See G below.
5. `halt.jlink` -> `stub_state=PARKED`; LAUNCH again (no reload needed: the
   CRC'd image is immutable) -> `out_seq` +1, bars rotate by one.

The command files hold absolute local paths: generated, gitignored.

## Deviations from the plan text

- **Stub table maps SRAM1 MiB 1 WB-WA S=1 executable (MiB 2 WB S=1 XN)**,
  not Device. Sec 5 step 2 says "else Device", but the payload must execute
  its first instructions at 0x02500000 under the stub table (Device is XN),
  and the stub must read the image for the CRC. Same attributes as the
  renderer's own table plans for MiB 1-2, so switching tables never remaps a
  PA to a different memory type.
- **MRAM section 0x800 is WB, PL1 read-only, XN** (APX=1 AP=01). The stub
  never executes from MRAM after relocation; only the release copy reads it.
- **Mailbox init clears `ctrl_cmd` and the fault record** (not `ctrl_entry/
  len/crc`). A command left from before a reboot is dropped, not obeyed.
- **HALT while parked does not turn FAULT into PARKED.** FAULT means
  "parked after a fault"; LAUNCH is accepted from PARKED or FAULT; a
  successful LAUNCH clears the record. The M55 watchdog should treat
  PARKED and FAULT alike as "parked" and read `fault_*` before LAUNCH.
- **A fault writes `ctrl_cmd=HALT`**, so the sibling core's payload returns
  on its own. The first fault wins; later ones on either core do not
  overwrite it (fixes the probe's shared-record limitation). A two-core
  Peterson lock (NC flags + DSB, bounded 1 ms) keeps simultaneous faults
  from interleaving fields.
- **The previous boot's record survives** in `pad4[0..3]`
  (`MBOX_OFF_LAST_FAULT_*`: core, code, lr, dfar), copied at init only when
  the magic was already valid (warm A32 restart); zeroed on cold SRAM.
- **LAUNCH timing stamps** in `pad4[4..7]` (`MBOX_OFF_T_*`, CNTVCT low
  32 bits): release copy start/end (0 on a dev boot), `launch()` entry,
  payload jump. The HE logs them with the renderer's own stamps at the
  first frame (`src/platform/a32.c`); `decode.py` prints them.
- The plan's `0x02402000` "stub status + core-1 park loop + fault scratch"
  block is not used: stub-private state lives in the stub image's own
  `.bss` (NC, MiB 0), core 1 parks in C in the stub copy.

## Sequence self-review

### Reset and relocation (`reset_handler`, core 0, from 0x80020000)

1. `adr r0,_vectors` -> VBAR = 0x80020000 (MRAM vectors, PC-relative
   branches, so they work there). SCTLR.V=0 (VBAR honoured) and **TE=0**
   (new vs the probe: TE=1 would take exceptions in Thumb and run our ARM
   vectors as garbage; the probe never faulted so never tested it). ISB.
2. SP = 0x02420000.
3. `cache_sync_all` BEFORE the copy: DCCISW every level to LoC, then
   ICIALLU + BPIALL, DSB, ISB. The routine is in `.text` but position
   independent (register ops + PC-relative literals), so `bl` reaches the
   MRAM copy. Done first so that no dirty line from earlier software can be
   evicted later on top of the copied stub (the copy itself does not
   allocate: MMU off => data accesses are Device, uncached).
4. Word copy [0x80020000, +image) -> 0x0240C000, zero `.bss`. DSB,
   ICIALLU, BPIALL, DSB, ISB (I-cache may be on with the MMU off; the SRAM
   range must not hit stale lines), then `ldr pc,=relocated`.
5. `relocated`: `cpu_setup` (VBAR = 0x0240C000, V/TE, CPACR, ISB,
   FPEXC.EN, FPSCR=0, CNTKCTL EVNTEN|EVNTI=15, ISB), `stub_build_table`
   (C, after NEON is on so -O2 NEON code would be safe; it is scalar),
   `mmu_on`, `stub_main`.
6. Faults: before step 4's jump the vectors run from MRAM and
   `fault_common` sees PC >= 0x80000000 -> `early_fault` copies a 10-word
   park page (8 vectors + WFE loop) to SRAM0 `0x023FE000`, records
   {"EFLT", core, code, lr} at `0x023FE040`, points VBAR there, THEN tries
   the mailbox (an SRAM1 fault now lands in the SRAM0 loop) and parks in
   SRAM0 -- never in MRAM. Only if the fault LR is inside `early_fault`
   itself (SRAM0 unwritable) does it WFE in MRAM: nothing writable exists. After
   relocation but before the table exists (`table_ready`=0) -> `stub_fault`
   records and WFE-loops. After that -> recorded, then the re-entry path.
   A second fault on the same core before it reaches its park loop (i.e.
   the re-entry path itself faults) -> WFE-loop, never a fault storm.

### Table (`stub_build_table`, MMU off)

All 4096 entries Shareable Device XN, then: 0x020-0x023 Normal NC XN (FBs;
0x02380000-0x02380FFF is inside 0x023 and simply never accessed);
0x024 Normal NC exec; 0x025 WB-WA S=1 exec; 0x026 WB-WA S=1 XN;
0x027 descriptor 0 (translation fault, TF-A RW 0x027DE000-0x027ED000);
0x800 WB PL1-RO XN. AP=11/TEX encodings are the probe's (TRE=0, AFE=0,
both forced in `mmu_on`). Descriptors checked in the disassembly:
0x1C12/0x1C02/0x11C0E/0x11C1E/0x941E/0x10C16. DSB, `table_ready=1`.
Table memory is NC and TTBR0 walk bits are 0 (NC walks): no maintenance
needed for the walker to see it.

### `mmu_on` (all entry paths)

TTBR0 = 0x02404000, TTBCR = 0, DACR = 0x55555555, ISB, TLBIALL, ICIALLU,
BPIALL, DSB, ISB, SCTLR: clear A/WXN/UWXN/EE/TRE/AFE, set M/C/Z/I, ISB.
With the MMU off it enables; with it on (re-entry from a payload table) it
switches tables. Both are safe because every caller runs from 0x024xxxxx,
VA==PA in the stub table and, by contract, in any payload table. TLBIALL
after the TTBR0 write drops the payload's (global, nG=0) entries.

### CPU_ON (`stub_main`)

Mailbox init first (state PARKED, core1 OFF, heartbeats 0, ctrl_cmd 0,
fault record 0, version, DSB, magic, DSB). Then SMC32 `0x84000003`,
r1 = **0x00000001** (Aff0=1, bit31 masked), r2 = `stub_core1_entry`
(SRAM address), r3 = 0. rc != 0 -> fault PSCI (lr = rc). Then WFE-wait up
to 100 ms (CNTVCT, wrap-safe subtraction) for core 1 PARKED, else fault
PSCI lr=1. Core 1: SP 0x02424000, `cpu_setup` (its own VBAR, NEON, FPSCR,
CNTKCTL -- all per-PE), `mmu_on` (same table), `stub_core1_main`: samples
the go counter, DSB, PARKED, DSB, SEV, park. No set/way on core 1 at
start (probe rationale: its L1 is reset-clean, L2 was cleaned by core 0).

### Park loops

Core 0: heartbeat0++, read `ctrl_cmd`; non-zero -> clear it (consumer
clears), DSB; LAUNCH -> `launch()`, anything else ignored; WFE. Core 1:
heartbeat1++, `core1_go != seen` -> go; WFE. All state they touch is NC
(mailbox, stub `.bss`, stacks in MiB 0) -- no cache maintenance anywhere
in the loops, debug AP sees it directly. WFE wakes on the event stream
(EVNTI=15) and on SEV. Race fixed during self-review: core 1 samples `seen`
BEFORE publishing PARKED; sampling after would miss a go that core 0 sends
the instant it sees PARKED.

### LAUNCH (`launch`, core 0)

1. Read ctrl_entry/len/crc once. core 1 RUNNING -> CORE1_BUSY.
   Range: entry 4-aligned, 0x02500000 <= entry, entry+len <= 0x02580000,
   len != 0 -> else BAD_RANGE (lr=len, dfar=entry).
2. DCIMVAC over [entry, +len) (line from CTR.DminLine), DSB. The image was
   written behind the caches (debug AP / M55); a clean line cached from
   before would make the CRC read stale bytes. Invalidate, not clean: the
   invariant below guarantees no dirty lines exist here, and if one did,
   memory (the new image) is the truth. (If the core upgrades DCIMVAC to
   clean+invalidate, the invariant still makes it harmless.) By-VA ops on
   S=1 memory are broadcast to core 1.
3. CRC32 (same C as the host test) != ctrl_crc -> BAD_CRC (lr=computed).
4. Clear fault record. `cache_sync_all`: DCCISW all levels, ICIALLU,
   BPIALL, DSB, ISB (core 1 is parked in NC code: no concurrent set/way).
5. stub_state=RUNNING. If core 1 PARKED: core1_entry, core1 state RUNNING
   (core 0 owns PARKED->RUNNING, so a payload's "wait for core 1 to park"
   can never see a stale PARKED), DSB, go++, DSB, SEV.
6. `payload_jump(entry, 0)`: SP = stub stack top, r0 = core, r1 = mbox,
   lr = re-entry address, FPSCR = 0, DSB, ISB, `bx`.
7. Core 1 on go: DSB, read entry, ICIALLU + BPIALL + DSB + ISB (its own
   I-side; ICIALLU is not broadcast), `payload_jump(entry, 1)`. D-side:
   coherent via SMPEN/S=1, and the image is at PoC.

**Invariant:** every path back into the stub runs DCCISW, so a parked stub
never has dirty lines in SRAM1 MiB 1-2. That is what makes the invalidate
in step 2 and a debug-AP reload while parked safe.

### HALT / re-entry

Core 1 (`0x0240C024`): `cpsid aif` + SVC, SP, `cpu_setup`, `mmu_on`,
`cache_sync_all` (its L1 + L2), sample go, DSB, PARKED, DSB, SEV, park.
Core 0 (`0x0240C020`): same prologue, then WFE-wait <= 1 s while core 1 is
RUNNING (core 1's set/way must finish before core 0's touches the shared
L2, and core 1 must be out of the payload before anything replaces it;
timeout -> CORE1_TIMEOUT), `cache_sync_all`, clear a leftover HALT,
RUNNING -> PARKED, park. Core 1's set/way can overlap core 0 still running
the payload (it returns first by contract): set/way is non-destructive
(clean+invalidate), and anything core 0 still holds is swept by core 0's
own pass afterwards.

### Faults

Vector -> r0=code, r1=banked LR, core from MPIDR.Aff0 -> SVC -> **`mmu_on`
first** if the table exists (no stack needed; the payload's table may be
live, and no stack push or mailbox write may happen under it) -> the core's
stub stack -> `stub_fault`: DFSR/DFAR/IFSR/IFAR (not mode-banked),
first-fault-wins record, stub_state=FAULT, ctrl_cmd=HALT, DSB, SEV -> the
core's re-entry path. VBAR always points at SRAM once relocated -- the
MRAM image is never an exception target after the jump.

### Release mode

Header at image +0x28 `{payload_off, payload_len, payload_crc}` +
"STUB" mark at +0x34, zero in the dev build. Non-zero: bounds-check
(off >= 0x40, aligned, inside the one mapped MRAM MiB, len <= 512 KiB,
else BAD_HEADER), word-copy MRAM -> 0x02500000 through the WB mapping,
**DCCMVAC the range** (LAUNCH's invalidate would otherwise throw the copy
away), set ctrl_entry/len/crc, `launch()`. `mkpayload.py release` builds
the image; verified: stub + colour-bar = 3,688 B, payload_off 0xCC0.

## Needs silicon proof (CP-A2)

1. Relocation + DCCISW-before-copy from MRAM with MMU off; stub reaches
   PARKED with magic/version in the mailbox (first time the stub, not the
   probe, is A32_APP).
2. Both heartbeats advance (core 1 via CPU_ON at an SRAM1 entry).
3. LAUNCH of `colorbar.bin` from the debug AP with no core halt; bars on
   glass with the HE display-only image; `out_seq`/`out_heartbeat`.
4. HALT -> PARKED -> LAUNCH repeatable without reload (bars rotate).
5. LAUNCH latency: the CRC runs from NC (uncached) stub code; 424 B is
   nothing, a 512 KiB renderer image is estimated tenths of a second --
   measure before T-A5 (`ponytail:` byte table / run the CRC WB if slow).
6. The MHU0-window read (G): values or DABORT / async abort (read only).
7. A deliberate fault payload (e.g. read 0x02700000) -> FAULT record with
   DFAR, parked, LAUNCH recovers. Also proves SCTLR.TE handling.
8. Wrong CRC / out-of-range entry -> BAD_CRC / BAD_RANGE, still parked.
9. Release mode is built and host-tested only; not on silicon until T-A9.

## Pre-flash review of 00040f5 -- fixes

- **A (major)** `fault_common` switched to the stub table (`mmu_on`, stackless,
  only once `table_ready`) before the stack or the mailbox is touched.
- **B** pre-relocation faults park in SRAM0 `0x023FE000` (see reset step 6),
  record there first, then the mailbox.
- **C** a second fault on the same core still WFE-stops (in SRAM1), but core 1
  now reports `stub_core1_state=OFF`, so core 0's return does not wait for
  it and the next LAUNCH runs single-core instead of failing CORE1_BUSY
  forever. Core 0 double-faulting leaves FAULT and needs a reset: nothing
  else could service a LAUNCH.
- **D** LAUNCH command file: HALT, bounded logged wait, image, ctrl words,
  `ctrl_cmd` last (dev loop step 3).
- **E** fault record under a two-core lock; previous boot's record kept in
  `pad4[0..3]`.
- **F** contract: 0x024xxxxx must stay Normal NC, VA==PA, executable in
  every payload table (`stub_abi.h`).
- **G** colour-bar fills FB A AND FB B (FB B bar order +4), then reads three
  words of FB B inside the TF-A MHU0 window (`0x02380000`, `0x02380800`,
  `0x02380FFC`) and records in `pad3[]`: [0] progress 0xCB0B0001..3,
  [1..3] read, [4..6] written, [7] ISR (bit 8 = async abort pending; CPSR.A
  is masked so an external abort would otherwise be silent). A synchronous
  abort -> stub FAULT DABORT with DFAR, progress stuck at 2. All three
  equal -> NS A32 writes to the window stick. `decode.py` interprets it.

## Delta review of f32b59e -- colour-bar BLOCKER

G wrote FB B across the TF-A MHU0 window and could corrupt TF-A/SE
messaging. Now: the FB B fill skips exactly [0x02380000, 0x02381000)
(`STUB_MHU0_WINDOW`, per-word check), the three window words are only READ
(`pad3[1..3]`, `pad3[4..6]` = 0, ISR in `pad3[7]`), and the exclusion is a
hard rule in `stub_abi.h` (contract) and next to `TR_FB_B` in
`src/ipc/tr_mbox.h`. Whether writes stick there is deliberately not tested;
T-A5 decides whether FB B moves. `colorbar.bin` 424 B, CRC 0x03075E84. The
stub binary is unchanged (defines/comments only).

## CP-A4 echo payload (`a32/payload-echo/`)

Silicon (2026-09-22, 2026W36-0009): stub f32b59e resident, relocated, both cores
parked at ~3000 heartbeats/s; colour-bar 0ab3c8b drew the first A32 pixels.
The HE A32-mode image then logged "no out_seq for 100 ms" because the
colour-bar publishes `out_seq = old + 1`, not `in_seq` -- hence this payload.

Core 0 only; core 1 returns to its park (shared `payload_start.S`). Loop:
HALT check (consumer clears, return to the stub) -> `out_heartbeat++` ->
`tr_mbox_take_in(m, last, ...)` from `src/ipc/tr_mbox.c` (linked as is,
`dsb sy` barrier; `last` starts at `out_seq`, so a frame the M55 published
before LAUNCH is still served) -> nothing new: WFE (event stream) ->
`in_fb` must be TR_FB_A or TR_FB_B, else not written and `out_dropped++`
(published anyway so the M55 never stalls) -> solid fill with
`colour[in.tick % 8]`, 4 x `vst1q_u16` (64 B) per iteration, the MHU0 window
[0x02380000, 0x02381000) split out of the FB B range exactly -> `dsb sy` ->
`tr_mbox_publish_out(out_seq = in seq, out_fb = in_fb, out_ticks0 = CNTVCT
ticks of fill + dsb, out_frames++)`.

Stats in `pad3[]`: [0] 0xEC0E0001, [1] min, [2] max, [3] mean (since this
LAUNCH; 64-bit sum, hence `-lgcc` for `__aeabi_uldivmod`), [4] fills,
[5] last `in.tick`. CNTVCT ticks, 10 ns. `decode.py` prints them in ms.
Expectation from the probe (NC NEON fill 1859.49 MB/s): ~1.0 ms per
1,843,200 B frame. `echo.bin` 2,336 B, CRC 0x1437CE34.
