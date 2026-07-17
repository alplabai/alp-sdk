# Debugging E1M-AEN801 (Alif Ensemble E8)

How to attach a debugger to an E1M-AEN801 module, the traps that catch
people the first time, and what J-Link is (and is not) for on this part.
This doc is about **attaching and reading state**, which is fully settled
below — for bench bring-up walk-through see
[`bring-up-aen.md`](bring-up-aen.md). Writing your own application image
into MRAM is a separate question this doc does not settle (§3).

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

## 2. Why does a manual J-Link connect fail when I select the Alif part number?

If you drive J-Link Commander (`JLinkExe`) yourself instead of `west
debug`, and pick the Alif part-number device (`AE822FA0E5597LS0_M55_HE`)
for the connect, it can fail — `Could not connect to the target device` —
even though the board is fine. That is exactly why `west debug` on this
board is wired to the **generic** device instead: the AE822's Secure
Enclave gates the part-specific access port, so only the generic
`Cortex-M55` device reliably reaches the released core for attach/debug.
Manual equivalent of what `west debug` runs:

```sh
JLinkExe -device Cortex-M55 -if SWD -speed 4000
```

A correct attach reports SW-DP IDR `0x4C013477` and CPUID `0x411FD220`
(Cortex-M55 r1p0). Any other IDR means you're on the wrong target or the
SWD wiring is reversed.

(With a recent-enough J-Link DLL, the part-number device profile *can*
also connect — but that's for a different purpose, see next section, not
an alternative attach device. For attach/debug, always use the generic
device, which is what `west debug` already does.)

## 3. Flashing your application — not yet documented here

This doc deliberately stops at attach/debug. What the actual customer
flashing path looks like for E1M-AEN801 — whether J-Link alone can write
your application to MRAM, or whether the Alif SETOOLS toolchain is
required — is still being pinned down against real silicon as of this
writing, and earlier drafts of this section stated both answers at
different points. Rather than publish an unverified claim, this section
is left as a **known gap**: see [`aen-provisioning.md`](aen-provisioning.md)
and [`aen-bench-bringup.md`](aen-bench-bringup.md) for the current state
of that flow, and check back here once it's settled.

What IS verified, independent of that open question: attaching a debugger
(§1–§2, generic `Cortex-M55` device) never writes MRAM and needs none of
the flashing tooling discussed in those other docs.

## 4. My board looks dead — J-Link won't attach at all

Symptom: some time after boot, J-Link reports `Failed to power up DAP`
(or SETOOLS reports `Target did not respond`) even though the probe
otherwise enumerates fine.

Cause: once the resident application returns from `main()` and enters an
idle / low-power wait, the Secure Enclave gates the debug power domain
off (and, separately, the SE-UART maintenance channel goes to sleep too).
**The board is not bricked** — it's simply not attachable while the
resident app is idling.

Fix: catch it in the boot window. Power-cycle or reset the board and
attach within the first couple of seconds after reset/power-on, before
the resident application reaches its idle loop — the debug port (and the
SE-UART) are reachable in that window. If you need the debug port to stay
reachable indefinitely, flash a build that stays busy and never idles
(never calls `WFI`); once it's running, the DAP stays powered.

## 5. How do I see program output?

However your application gets onto the board (§3), reading its output
does not need a debugger. As shipped, the E1M-AEN apps default to the Alp
UART console on the carrier's console UART; attach a 115200 8N1 serial
terminal to the carrier's console header and `printk()`/application
output appears directly.
