# A32 bare-metal memory probe

First numbers from the Cortex-A32 pair (E1M-AEN803 2026W36-0009) for scalar,
NEON and D-cache-clean throughput, PMU clock speed, dual-core coherency, and
CDC200 NS-access reachability -- feeding the T-A0 open questions in
`../../docs/superpowers/plans/2026-09-22-a32-renderer.md`. No OS, no libc: this is
a bare-metal payload that TF-A jumps to directly.

Stage 1 = single core (core0), SRAM0 only. Stage 2 = adds core1 (PSCI
CPU_ON), the PMU, non-fatal CDC200 register reads, and SRAM1. Stage 1's
tests, results layout and addresses are **unchanged** by stage 2 -- every
stage-2 field is appended after the stage-1 layout, never inserted into it.

## What it is

- `start.S` -- ARM-state reset entry at `0x80020000`: sets `VBAR`, invalidates
  I-cache/branch-predictor/TLB, clean+invalidates D-cache (set/way, all CLIDR
  levels), builds a short-descriptor 1 MiB-section page table at `0x023E0000`
  covering SRAM0+SRAM1+MRAM, enables the MMU/caches, enables NEON, installs a
  heartbeat-incrementing park loop in SRAM (core0: `0x023FE000`; core1, once
  it's up: `0x02402000`), and calls `main()`. Also: the non-fatal-fault
  recovery mechanism (`probe_read32`/`pmu_try_read`), PSCI CPU_ON
  (`psci_cpu_on_secondary`), the LDREX/STREX helper (`atomic_increment_n`),
  and core1's own entry point (`secondary_entry`).
- `main.c` -- stage-1 tests (scalar fill, NEON fill, NEON read/xor, NEON
  copy, D-cache clean) over the NC and WB SRAM0 buffers; stage-2 tests (PMU
  clock, dual-core fill/clean/read + LDREX/STREX + cross-L1 coherency, CDC200
  reads, mailbox sentinel). All timed with `CNTVCT`, results written to a
  fixed block at `0x023FF000`.
- `results.h` -- the C-side view of that results block (offsets asserted at
  compile time).
- `link.ld` -- places the image at `0x80020000`, XIP from MRAM; fails the
  build if `.data`/`.bss` are non-empty.
- `decode.py` -- host-side: turns a raw dump of `0x023FF000..+0x350` into a
  per-test MB/s + stage-2 report.

See `../../docs/2026-09-22-a32-probe-spec.md` for the full spec (memory plan,
exact MMU attribute encoding, results layout) and
`IMPLEMENTATION-NOTES.md` for the line-by-line design record, deviations,
and self-review of the cache/MMU sequence.

## Build

GNU make; on Windows, run this in WSL2.

<!-- cross-platform-lint:ignore -->
```
cd a32/probe
make
```
<!-- cross-platform-lint:resume -->

Produces `a32_probe.bin` (flat binary to replace `xipImage.bin` in the ATOC
entry `A32_APP`), `a32_probe.map`, and a `size` summary. Uses
`arm-none-eabi-gcc` 13.3.1 with
`-mcpu=cortex-a32 -marm -mfpu=neon-fp-armv8 -mfloat-abi=hard -O2
-ffreestanding -nostdlib -nostartfiles -Wall -Wextra` (zero warnings).

`make vectors` disassembles the first 16 instructions at `0x80020000` for a
quick sanity check without opening the full `.map`.

## What the bench reads

After core0 reaches `stage = 0xD0E` (or a fault handler parks it),
J-Link-read `0x023FF000..0x023FF350` (`sizeof(results_t)` minus the
start.S-internal probe-recovery handshake fields) into a file and run:

```
python3 a32/probe/decode.py dump.bin
```

It prints `magic`/`stage`/fault fields (if any), the `SCTLR`/`CPACR`/`FPEXC`
readbacks, one line per test (buffer, op, bytes, ticks, MB/s), and -- if the
dump covers the stage-2 region -- PMU MHz, PSCI CPU_ON result, dual-core
aggregate bandwidth, LDREX/STREX pass/fail, cross-L1 coherency pass/fail, and
the CDC200 register reads. `python3 a32/probe/decode.py --selftest`
exercises the same decoder against synthetic in-process blocks (no hardware
needed) and checks the MB/s math plus every stage-2 pass/fail path.

**Live heartbeats** (poll these directly over SWD -- they are NOT part of
the results-block snapshot, and keep advancing after the probe finishes, so
the bench can tell "parked and idle" from "wedged" e.g. after an HE Flow-C
reset):
- core0: `0x023FE018` (SRAM0, in `PARK_LOOP_SRAM`'s copy of `start.S`'s
  `heartbeat_park_template`)
- core1: `0x02402018` (SRAM1, `CORE1_PARK_ADDR`'s copy), **only if core1
  came up** -- check `psci_cpu_on_ret`/`secondary_mpidr` in the results
  block first

**Mailbox sentinel** (addendum item E): `0x02401000` holds `0x54524D42`
(`"TRMB"`) once `mailbox_sentinel_written=1` in the results block -- read it
directly to check HE/debug-AP visibility of SRAM1 with the A32 chain
resident, independent of the results block.

## Stage-2 memory map (SRAM1, `0x02400000-0x027FFFFF`)

| range | attribute | contents |
|---|---|---|
| `0x024xxxxx` | Normal NC, **executable** | `0x02401000` mailbox sentinel; `0x02402000` core1's park loop + heartbeat (`+0x18`) |
| `0x025xxxxx` | Normal WB-WA, **Inner Shareable (S=1)** | `0x02500000` dual-core fill/clean/read buffer, 1 MiB (core0: low half, core1: high half) |
| `0x026xxxxx` | Normal WB-WA, **S=1** | `0x02600000` LDREX/STREX shared counter; `0x02601000` 64 KiB cross-L1 coherency buffer; `0x02611000` its ready flag |
| `0x027xxxxx` | **unmapped** (Device/XN default) | TF-A RW lives at `0x027DE000-0x027ED000` in here -- never touched |

Stage-1's WB buffer (`0x02100000`, S=0) is untouched and still used only by
the single-core stage-1 tests, so those numbers stay comparable to what was
already measured on silicon (see `IMPLEMENTATION-NOTES.md`, "addendum item
A"). The stage-2 dual-core tests use the separate S=1 buffer at `0x02500000`
instead.

## The TF-A boot contract this relies on

All from `../../docs/2026-09-22-a32-probe-spec.md`, "Boot contract" (sourced from
TF-A `alif_lts-v2.10.8`, `PLAT=devkit_e7`):

- SE releases `A32_0` into TF-A `sp_min`; `sp_min` erets to BL33 at
  `PRELOADED_BL33_BASE = 0x80020000` (XIP from MRAM), **ARM** state, **SVC**,
  **Non-secure**, IRQ/FIQ/Abort **masked**, MMU/caches **off** (NS `SCTLR`
  unprogrammed). No image header is parsed -- the first word at
  `0x80020000` is executed directly, which is why it must be a valid ARM
  instruction (the reset vector's `b reset_handler`).
- `CNTFRQ = 100 MHz`; use `CNTVCT` (`mrrc p15, 1, r0, r1, c14`) for timing.
- TF-A secure MHU0 payload window `0x02380000..0x02380FFF` is never touched
  by this probe (it sits inside the one 1 MiB SRAM0 section we do map, but
  we never issue a load/store/fetch at that address).
- ATOC entry `A32_APP` (`cpu_id A32_0`, `mramAddress 0x80020000`) --
  `a32_probe.bin` replaces `xipImage.bin` there; nothing else in the ATOC
  changes.
