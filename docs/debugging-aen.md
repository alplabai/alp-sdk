# Debugging E1M-AEN801 (Alif Ensemble E8)

How to attach a debugger to an E1M-AEN801 module, the traps that catch
people the first time, and what J-Link is (and is not) for on this part.
This doc covers **attaching/reading state** (§1–§2, §4) and **flashing
your own application image** (§3) — for bench bring-up walk-through see
[`bring-up-aen.md`](bring-up-aen.md).

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

The same pre-idle-window logic applies to the SE-UART/SETOOLS
maintenance channel: forcing a fresh boot and catching the SETOOLS
handshake in that same early window recovers it the same way.

## 5. How do I see program output?

However your application gets onto the board (§3), reading its output
does not need a debugger. As shipped, the E1M-AEN apps default to the Alp
UART console on the carrier's console UART; attach a 115200 8N1 serial
terminal to the carrier's console header and `printk()`/application
output appears directly.
