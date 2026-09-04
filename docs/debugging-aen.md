# Debugging E1M-AEN801 (Alif Ensemble E8)

How to attach a debugger to an E1M-AEN801 module, the traps that catch
people the first time, and what J-Link is (and is not) for on this part.
This doc covers **attaching/reading state** (§1–§2, §4), **flashing your
own application image** (§3), **reading its output** (§5–§6), and **a
board that boots nothing at all** (§7) — for bench bring-up walk-through
see [`bring-up-aen.md`](bring-up-aen.md).

## 1. Attach a debugger: `west debug`

For the qualified board target (e.g.
`alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`), a J-Link debug runner
is wired for you out of the box:

```sh
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <your-app>
west debug        # or: west attach   (attach without resetting first)
```

`west debug` drives J-Link against the **generic `Cortex-M55`** device at
`--speed=4000` — that device/speed pair is set by the board definition
itself, so you don't need to pick a device manually. This needs no MRAM
write and does not go through SETOOLS.

Prefer `west attach` over `west debug` on a board that's already running:
a J-Link `reset` can perturb the SES / boot state (the exact
`AIRCR.SYSRESETREQ` reset scope — SES vs. M55-only — is unresolved, see
[`aen-bench-bringup.md`](aen-bench-bringup.md)) — `west debug` resets
before attaching, so it risks that disturbance; `west attach` does not.

## 2. The Alif part-number device vs. the generic device

If you drive J-Link Commander (`JLinkExe`) yourself instead of `west
debug`, and pick the Alif part-number device (`AE822FA0E5597LS0_M55_HE`)
for the connect, it can fail on an **older J-Link DLL** —
`Could not connect to the target device` — even though the board is
fine. The fix is to update J-Link (DLL V9.46+, confirmed working through
V9.50), not to avoid the part-number device: on a current DLL it connects
fine, and it is in fact *required* to unlock the part's built-in MRAM
flash loader (§3). `west debug` is wired to the **generic** `Cortex-M55`
device regardless, because it needs no MRAM loader for attach/debug —
that's just the simpler, purpose-matched device for this job, not a
workaround for a broken part profile. Manual equivalent of what `west
debug` runs:

```sh
JLinkExe -device Cortex-M55 -if SWD -speed 4000
```

A correct attach reports SW-DP IDR `0x4C013477` and CPUID `0x411FD220`
(Cortex-M55 r1p0). Any other IDR means you're on the wrong target or the
SWD wiring is reversed.

With a J-Link DLL V9.46+, the part-number device profile also connects
fine — but reserve it for the Flow D MRAM loader (§3), since it has no
attach/debug advantage over the generic device and the generic device is
what `west debug` already drives.

## 3. Flashing your application

Replacing the resident slot0 application on E1M-AEN801 has two proven
paths, both of which persist across a cold power-cycle (a bare
`loadbin` with no signed ATOC does **not** persist — see below):

- **J-Link, two-blob MRAM loader.** With a J-Link DLL V9.46+ and the
  part-number device (`AE822FA0E5597LS0_M55_HE`, §2), J-Link's built-in
  Alif MRAM loader writes the application blob to its slot0 address plus
  a separately-staged, signed `AppTocPackage.bin` to its package address.
  Both blobs must land — writing the application alone, without the
  signed ATOC (e.g. the output of a plain `west flash -r jlink`), is
  read back correctly by `verifybin` but does **not** commit and reverts
  on the next cold power-cycle.
- **Alif SETOOLS over the SE-UART.** `app-gen-toc` + `app-write-mram`
  drive the same commit through the Secure Enclave's maintenance
  channel; this is what `west flash` (the `alif_flash` runner) wires by
  default on this board.

**Both paths need SETOOLS to sign the ATOC** (`app-gen-toc`) before
either write — the J-Link *write* itself is SETOOLS-free, but producing
a valid, signed `AppTocPackage.bin` is not. There is no "stock J-Link,
no SETOOLS" flashing path.

**Known gap:** there is no serial/DFU recovery path (no mcumgr / MCUmgr
image-management / serial-recovery Kconfig is enabled on any E1M-AEN801
example or board default today). A SETOOLS-free field-update path is a
plausible future addition, not a shipped one.

**Recovery caveat:** if your own resident application gates the debug
port by idling (§4), a J-Link reflash is blocked until you either catch
the boot window (§4) or fall back to the SE-UART SETOOLS path, which is
gated by the same idle window but recovers the same way.

See [`aen-provisioning.md`](aen-provisioning.md) and
[`aen-bench-bringup.md`](aen-bench-bringup.md) for the full flashing
recipes and bench detail.

What IS verified, independent of which flashing path you pick: attaching
a debugger (§1–§2, generic `Cortex-M55` device) never writes MRAM and
needs none of the flashing tooling discussed in those other docs.

## 4. My board looks dead — J-Link won't attach at all

Symptom: some time after boot, J-Link reports `Failed to power up DAP`
(or SETOOLS reports `Target did not respond`) even though the probe
otherwise enumerates fine.

If instead the DAP stays perfectly healthy and the cores simply never
start — reads work, `VTOR` stays `0`, nothing boots — that is a
different failure with a different fix; see §7.

Cause: once the resident application returns from `main()` and enters an
idle / low-power wait, one mechanism gates both channels — the Secure
Enclave gates the debug power domain off AND the SE-UART maintenance
channel stops responding, at the same time. **The board is not
bricked** — it's simply not attachable while the resident app is idling.

Fix: catch it in the boot window — but a *fresh* `JLinkExe` reliably
misses it. The window is short (roughly 0.8–2.6 s after reset/power-on)
and a fresh `JLinkExe` invocation spends part of that budget on its own
probe-init, so "power-cycle and attach within a couple of seconds"
starting a new `JLinkExe` each time is not reliable. The technique that
actually lands the window:

1. Start **one** `JLinkExe` session *before* powering the board on, with
   auto-connect disabled (`-AutoConnect 0`) so its own probe-init
   finishes while there's nothing to connect to yet.
2. Power the board on, then immediately (in that same pre-warmed
   session) issue `connect` repeatedly — no new `JLinkExe` process — until
   it reports the core identified (e.g. `Cortex-M55 identified`). Because
   probe-init is already done, this lands inside the window.
3. Once connected, halt and hold the core there — a halted core can't
   reach its idle loop, so the DAP stays powered for the rest of the
   session.
4. To fix it permanently rather than re-catching the window every time,
   flash a build that stays busy and never idles (never calls `WFI`);
   once that image is resident, the DAP stays powered across boots.
   **That busy loop interacts with the logging subsystem — see §6
   before you conclude the resulting silent console means a dead
   board.**

The same pre-idle-window logic applies to the SE-UART/SETOOLS
maintenance channel: forcing a fresh boot and catching the SETOOLS
handshake in that same early window recovers it the same way.

**In-session variant:** the symptom above is what a *fresh* attach sees.
If you are already attached and connected, and the resident app idles out
from under you mid-session, the signature looks completely different and
reads as a much worse failure:

1. `halt` either fails outright (`WARNING: CPU could not be halted`) or
   SUCCEEDS and lands in `sys_clock_isr` with `IPSR = 00F` (SysTick) — the
   expected capture for a WFI-idling core, since the tick ISR is the only
   window where it's awake enough to halt;
2. every subsequent memory access then fails (`Could not read memory.`),
   ending in `****** Error: Could not start CPU core. (ErrorCode: -1)`;
3. the Coresight scan itself then dies: `AP[0]: Skipped. Could not read
   CPUID register` / `Could not find core in Coresight setup`.

Same cause, same fix (catch the boot window, or flash a build that stays
busy and never idles). A cold power cycle only *looks* like it fixes this:
a busy resident image (e.g. the canonical `person_detect` slot0) keeps the
DAP powered, so cycling back onto that image recovers attach — but the
idle-gate itself never changed, and the same in-session symptoms return
the moment an idling image is resident again. The `sys_clock_isr` capture
is also useful diagnostic evidence on its own: against a busy park loop
the tick ISR is roughly a 1000:1 shot, so landing there on a `halt` is
itself strong evidence the core was idling, not stuck.

## 5. How do I see program output?

However your application gets onto the board (§3), reading its output
does not need a debugger. As shipped, the E1M-AEN apps default to the Alp
UART console on the carrier's console UART; attach a 115200 8N1 serial
terminal to the carrier's console header and `printk()`/application
output appears directly.

## 6. The console prints nothing, and the board is fine

This is the trap that costs an afternoon, because every instinct points
at the flash, the probe or the image — and all three are healthy.

**Symptom.** Zero bytes on the console. Not "the banner appears but my
log lines don't" — *zero*: no `*** Booting Zephyr OS build ... ***`, no
Alp SDK boot-identity banner (`Alp SDK <version>  |  <SoM or board>  |
…  |  (c) Alp Lab AB`, from `src/zephyr/alp_banner.c`), no `LOG_INF`
output, nothing, across a full capture and across a cold power cycle.

**The interaction.** Two correct pieces of advice collide:

- §4 above tells you to run an application that **stays busy and never
  idles**, because an idling M55 makes the Secure Enclave gate the DAP
  and the SE-UART. The usual shape is a `main()` that never returns and
  never yields — `for (;;) { k_busy_wait(1000); }`.
- Zephyr's own logging default is `CONFIG_LOG_MODE_DEFERRED=y`, which
  formats and writes records from the log processing thread
  (`CONFIG_LOG_PROCESS_THREAD` is `default y`). Without
  `CONFIG_LOG_PROCESS_THREAD_CUSTOM_PRIORITY=y` that thread runs at
  `K_LOWEST_APPLICATION_THREAD_PRIO`, strictly below `main`'s
  `CONFIG_MAIN_THREAD_PRIORITY=0`; time-slicing only rotates among
  *ready threads of equal priority*, so it does not rescue it.

A `main()` that never yields therefore never lets the log thread run,
and the queued records are never printed. The banner goes with them:
`CONFIG_LOG_PRINTK` is `default y if PRINTK`, so `printk()` — including
the Alp SDK banner — is routed through the same starved queue. That is
why the measurement is *zero* bytes rather than "banner, then silence",
and it is exactly why this reads as a dead board rather than as an
application problem.

**How to spot it in one attach.** A running-but-silent board and a
faulted board are trivially distinguishable over SWD. Attach (§1),
then:

```
halt
Reg PC
Reg IPSR
mem32 0xE000ED28 1
```

The starved-log signature, measured on E1M-AEN801 silicon
(issue [#1373](https://github.com/alplabai/alp-sdk/issues/1373)):

| Read | Value | Reads as |
|---|---|---|
| `PC` | an address inside `z_impl_k_busy_wait` | the core is executing the delay loop |
| `IPSR` | `000` | `NoException` — not in a fault handler |
| `CFSR` @ `0xE000ED28` | `00000000` | no configurable fault has ever latched |

`PC` is a per-build address, so resolve it against your own `zephyr.elf`
(`arm-zephyr-eabi-nm`/`addr2line`) rather than comparing it to a literal
from someone else's capture. `IPSR = 000` together with `CFSR =
00000000` rules out the fault explanation in a single read: the board is
running, fault-free, and simply has nothing draining its log queue.

**What the SDK does about it.** Every E1M-AEN board tree defaults the
logging mode to minimal rather than inheriting Zephyr's deferred
default:

```
# zephyr/boards/alp/e1m_aen*/Kconfig.defconfig
choice LOG_MODE
	default LOG_MODE_MINIMAL
endchoice
```

`CONFIG_LOG_MODE_MINIMAL` formats and writes in the calling context, so
a non-yielding `main()` cannot starve it and the busy-loop advice in §4
stays safe to follow. It is a board *default*, not a lock: an
application that wants the deferred pipeline — log backends, runtime
filtering, timestamps — sets `CONFIG_LOG_MODE_DEFERRED=y` in its own
`prj.conf` and gets it back, along with the responsibility to yield
(`k_msleep()`) somewhere in `main()`.

Minimal mode is not free of consequences, and they are worth knowing
before you override the default in either direction: it drops timestamps,
prefixes, colours, runtime filtering and the log backends entirely
(everything goes to `printk()`), and build-time filtering via
`CONFIG_LOG_DEFAULT_LEVEL` still applies. If your output is missing but
the banner *does* appear, you are past this section's failure and looking
at ordinary log-level filtering instead.

## 7. The Secure Enclave boots nothing at all — cores parked, VTOR 0

§4 is the case where a *running* application takes the debug port away
from you. This section is its opposite, and it is routinely mistaken for
a dead Secure Enclave: the DAP is perfectly healthy, but nothing ever
boots.

**Symptom.** After every cold power cycle: SW-DP IDR reads `0x4C013477`,
memory reads and writes work, the J-Link MRAM loader still programs and
verifies — and yet `VTOR` stays `0`, the cores are parked, and the
application console is silent. J-Link may also report `Could not find
core in CoreSight setup`.

**First prove your reads are real.** Read the same address twice. The
§4 gated-DAP state returns *different values on successive reads of the
same address* — floating reads that look like data. Two different values
means the debug power domain is gated and you are in §4, not here.

**Do not write anything to MRAM until you have done §7.2.** The
diagnosis depends on the current contents of slot0, and a SETOOLS erase
(`app-write-mram -e`) commits immediately and destroys that evidence.

### 7.1. Three things that look like evidence and are not

Each of these has sent someone down the "the Secure Enclave is damaged"
path:

- **"I restored my MRAM backup and verified it byte-identical."** An
  immediate readback is not proof of a commit. Whether a J-Link write
  persists depends on what is resident: with **no** bootloader to
  chainload it, an application blob written without a separately staged,
  signed `AppTocPackage.bin` reads back correctly through `verifybin`
  and through a plain memory read, and then reverts on the next cold
  power cycle (§3). On an Alp-Lab-provisioned module, where MCUboot is
  the factory ATOC, the opposite is bench-proven: an `imgtool`-signed
  image `loadbin`'d straight to slot0 is verified by MCUboot and
  chainloaded, and survives cold power cycles — that is
  [`aen-provisioning.md`](aen-provisioning.md) §0.5 Option B. **Either
  way, the only proof of a commit is a cold power cycle followed by a
  re-read** — a readback taken in the same session as the write
  establishes nothing about what is actually in MRAM now.
- **"I cannot read above `0x80580000`, so I cannot inspect the SE."**
  That system area is SE-read-gated on a healthy part. Being unable to
  read it is expected. Note that this cuts both ways: because it is
  gated on good parts too, it carries no diagnostic weight in either
  direction.
- **"There is no SE banner on the application console."** There never
  is, for two separate reasons. The boot output is split across two
  ports — the SES header comes out on the **SEUART**, application output
  on the application console — and the application console is only ever
  driven by an application that was built with a console on it. A silent
  application console is consistent with a healthy SE, a missing
  application, *and* a perfectly good application that simply has no
  UART console configured.

  Before you read anything into a silent console, **confirm which port
  you are actually on** — see §7.1.1.

#### 7.1.1. Confirming you are on the application console

Three independent things must all be true, and each has been wrong in
the field:

1. **The right UART for your carrier.** On the E1M carrier the HE
   application console is **Alif UART5** — the E1M edge **"UART0"** — on
   **P3_4 (`UART5_RX_A`) / P3_5 (`UART5_TX_A`)**, brought out on the
   E1M-EVK as header **J17**. On an Alif Ensemble DevKit it is a
   *different* UART: **UART2**, on **P1_0 / P1_1**. A tap placed by
   DevKit habit on an E1M carrier is silent no matter how healthy the
   board is.
2. **The application actually routes its console there.** In the board
   tree that means `zephyr,console = &uart5` (and `zephyr,shell-uart`),
   with `CONFIG_UART_CONSOLE=y`. An application built for the RAM console
   instead (`CONFIG_RAM_CONSOLE=y` with `CONFIG_UART_CONSOLE=n`) writes
   nothing to any UART by design. If you maintain your own board port,
   check this in *your* tree, not in ours.
3. **The right line settings.** The application console is
   **115200 8N1** (`current-speed = <115200>`). The SEUART is a
   different rate entirely — **57600** on E8/E6/E4 — so the baud you are
   using is itself a strong hint about which port you are on.

Beware the reverse mix-up too: on a setup whose only USB serial adapter
is the SE-UART's, *neither* application UART is wired out to USB, so a
serial port that "just appeared over USB" may well be the SEUART rather
than the application console.

What a healthy application console prints on this SDK is the boot-identity
banner from `src/zephyr/alp_banner.c`, in the shape:

```
Alp SDK <version>  |  E1M-AEN801  |  Alif Ensemble E8  |  (c) Alp Lab AB
```

If what you remember seeing on that port does not look like that, it did
not come from this SDK, and identifying what produced it is worth doing
before drawing conclusions from its absence.

### 7.2. The read that discriminates

This writes nothing, and it comes first.

Before it, if the board has been through a long flashing session, power
it **off for about 30 seconds** and back on. A tangled Secure Enclave
that refuses maintenance (`Target did not respond`, MRAM writes timing
out) has recovered from exactly that more than once; a brief cycle
restores only the SE-UART handshake. It costs nothing and it changes no
state.

Then listen on the SEUART — **57600 8N1** for E8/E6/E4, **55000** for
E7/E5/E3/E1 — and cold power-cycle the board during the capture. §2 of
[`aen-provisioning.md`](aen-provisioning.md) has the listener script and
the wiring rules (1.8 V logic level, crossed TX/RX, common ground); all
three bite in ways that look like a dead board.

| What the capture shows | Reads as | Next |
|---|---|---|
| SES header present (`SEROM v1.x.y`, `SES Ax v1.x.y`, `[SES] STOC DEVICE ok`, `[SES] LCS=1`) **and** `[SES] No ATOC` | the SE is alive and the application TOC is missing or invalid | reflash — §7.3 |
| SES header present **and** `[SES] M55-HE booted from address 0x58000000`, with no `[SES] No ATOC` line | the SE is alive, found a valid ATOC and released the core — so the failure is *above* the SES | capture the application console too — §7.2.1 |
| **no SES header at all**, at the correct baud, with the adapter proven good | the failure is below the SES; this is where a genuinely damaged SE state would land | §7.4 |
| zero bytes | inconclusive — do not diagnose the board yet | jumper the adapter's own TXD↔RXD and confirm it echoes (a dead-RX adapter loops back through its own ground and never hears the board), then re-check 1.8 V, crossing and ground |

#### 7.2.1. If the SES released the core, look at the application console

There is a documented cause of `VTOR = 0x00000000` with a live debug
port that has nothing to do with the Secure Enclave: **MCUboot refusing
the image in slot0.** Every E1M-AEN module ships with dev-signed MCUboot
as the factory ATOC, so a bad slot0 write leaves a perfectly valid ATOC
and a healthy SE while nothing boots.

The two signatures, both bench-observed, print on the **application**
console:

- `E: Unable to find bootable image` — a tampered or wrong signature.
- `E: Bad image magic 0x20004c60` — a non-MCUboot image (a raw
  `zephyr.bin` rather than a `zephyr.signed.bin`).

Both leave the debug port alive — `Secure debug: enabled`, the core
halts and single-steps through real instructions — which is exactly the
picture §7 opens with. A bad slot0 write does not brick J-Link access.

The fix is to re-sign and rewrite slot0 (`west flash`, or
[`aen-provisioning.md`](aen-provisioning.md) §0.5 Option B with a
`zephyr.signed.bin`), **not** the SETOOLS reflash in §7.3. See
[`secure-boot.md`](secure-boot.md).

If the application console is silent here too, then MCUboot itself is
not running and you are back to the ATOC question.

### Second probe

Worth running either way:

```bash
./maintenance -opt devenquiry -c <your-serial-device>
```

(`maintenance` is an Alif SETOOLS binary, run from your SETOOLS
directory. SETOOLS is license-gated by Alif and is not redistributed
with this SDK.)

`Device connected` proves the SES itself is up and talking, whatever the
application side is doing. Silence here at the correct baud, alongside a
missing SES header, is the combination that points below the SES.

### 7.3. Recovering when the SES is alive

The state to expect is a missing or invalid application TOC: the SES has
nothing valid to boot, so it never releases the cores. That is the same
state a factory-fresh board is in — §6 of
[`aen-provisioning.md`](aen-provisioning.md) notes that `Could not find
core in CoreSight setup` is *normal* before an application has been
provisioned. It is a routine reflash, not damage.

One way an already-working board arrives here is an interrupted SETOOLS
write, because the two halves are not symmetric: `app-write-mram -e`
commits immediately, while the program half commits **only on
completion**. Interrupt the program between those two — a `^C`, a closed
pipe, a `head` on the output, a reset landing mid-download — and slot0 is
left erased with a healthy SE that has nothing to boot.

Flash Alif's **stock blink** from the SETOOLS package first, before your
own image — it separates "can the SE be written at all" from "is my
application good". Write an **app-only** TOC and leave the factory
DEVICE config alone; a mismatched DEVICE config is itself a documented
crash cause. [`aen-provisioning.md`](aen-provisioning.md) §3–§5 has the
`tools-config` / `app-gen-toc` / `app-write-mram` sequence and the
app-only TOC JSON.

Three things to hold to while doing it:

- **Make sure the supply is solid before you start.** A sag mid-write is
  the one thing here that can turn a recoverable state into a worse one.
- **Let `app-write-mram` reach `100% ... Done`.** Do not pipe it into
  anything that can close early, and do not abort it because it looks
  stalled — `Waiting for Target..[RESET Platform]` is Hard-maintenance
  waiting for you to power-cycle the board, not a hang.
- **Confirm with a cold-cycle read, never with `verifybin` alone.** Read
  back the first words at the slot0 address after a cold power cycle and
  compare against the image you wrote.

SETOOLS is license-gated by Alif and is not redistributed with this SDK;
obtain it from Alif directly.

### 7.4. The SE is in Recovery — SEROM is alive, SERAM is not

A distinct state from §7.3, and a recoverable one. The tell is that the
device answers, but answers as SEROM:

```
[INFO] Connecting to target...Device connected in Recovery
```

`getbanner` returns that line instead of an `SES <rev> v<version>` line,
and a System Package update refuses to proceed:

```
Bootloader stage: SEROM
[ERROR] Please use Recovery option from ROM menu in Maintenance Tool
```

`app-write-mram` reports the same condition as a "device in SEROM
Recovery mode" warning — that warning is this state, and an application
write cannot clear it.

This is SEROM running with no valid SERAM to hand off to. The part is
alive, reachable, and recoverable in software.

**Cause.** An interrupted **System Package** write — the Secure Enclave
half of the hazard §7.3 describes for the application half: a `^C`, a
closed pipe (piping the tool's output into `head` or `less`), a `timeout`
expiring, or a reset landing mid-write. The two halves differ in what
they cost you: interrupting the application write leaves a healthy SE
with nothing to boot, and costs a reflash. Interrupting the System
Package write takes out SERAM itself and drops the part into Recovery.

#### 7.4.1. Recovering: the ROM menu

In Recovery the maintenance tool's top-level menu is **not** the menu a
healthy board shows — there is no Device Control entry, and the option
you need only exists here:

```
Available options:
 1 - ROM
 2 - Device Information
 3 - Utilities
 4 - Alt. Boot Octal SPI
```

Select `1 - ROM`, then `1 - MRAM Recovery`:

```
Available options:
 1 - MRAM Recovery
 2 - MRAM Recovery (No Reset)
```

The tool picks the System Package itself. It reads the part number and
silicon revision over SEUART and selects the matching file — you do not
choose between a `-dev` and a plain package, and the `rev-a0` / `rev-a1`
in the filename is the **silicon** revision matched to the part, not a
package version to pick the newest of.

The write takes roughly four and a half minutes on an AE822, and ends
with the SERAM offset record and a reset:

```
[INFO] recovery time:     269.79 seconds
[INFO] Writing offset '<offset file>' to address 0x5ffff0
[INFO] Target reset
Recovery process finished. Please reload maintenance tool for verification
```

**Let it finish.** Do not pipe it into anything that can close early, and
do not put a timeout shorter than about six minutes on it — that is
precisely the mistake that produces the state you are recovering from,
and doing it again here costs another recovery cycle rather than a
reflash.

#### 7.4.2. Verify before you reflash the application

Two reads, both of which must come back clean:

```sh
maintenance -opt getbanner    # expect: SES <rev> v<version> <build date>
maintenance -opt getclock     # expect: HFXTAL STARTED / PLL LOCKED
```

`getclock` should report `HFXTAL STARTED`, `PLL LOCKED`, and the M55-HP
at its full rate. Note that `SE CLOCK: HFRC` appears on a **healthy**
board — the SE runs on the RC oscillator by design, so it is not a fault
signature on its own. The fault signature is `HFXTAL OFF` with `PLL OFF`.

Only once both reads are clean, reflash your application per §7.3.

#### 7.4.3. If the tool cannot reach the target at all

If the maintenance tool never gets far enough to show a menu — `Target
did not respond` to `COMMAND_START_ISP`, or `Waiting for
Target..[RESET Platform]` that never advances — the SEUART channel is
waiting on a reset it is not getting. Supply one over SWD: a
connect-under-reset from your debug probe drives the reset the tool is
waiting for, after which the menu comes up and the recovery above
proceeds normally.

Confirm the probe is on the part you think it is before you do this: the
E8 answers SW-DP ID `0x4C013477`. If your probe reports a different DP
ID, you are attached to different silicon — stop.

### 7.5. When the SES never answers at all

If the SES header is absent at the correct baud with a proven-good
adapter, `devenquiry` is silent, **and** the maintenance tool does not
report `Device connected in Recovery` either, the fault is on the Secure
Enclave side and below the recovery path in §7.4. This SDK has no recipe
for that state, and we will not invent one: SE-side recovery and the
device lifecycle are Alif's domain, and the authoritative reference is
the **Alif Security Toolkit User Guide** that ships in the SETOOLS
package.

Capture the SEUART output and any tool error codes verbatim before doing
anything else, and take it to Alif support — or to us
(<contact@alplab.ai>, or an issue on this repo) and we will take it
there with you. Do not keep re-flashing in the hope that one write
lands; repeated write attempts add wear and destroy the evidence that
tells you what actually happened.
