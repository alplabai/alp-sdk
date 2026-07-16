# Provisioning + first-boot — E1M-AEN (Alif Ensemble)

How to load your first application onto an **E1M-AEN** SoM (Alif Ensemble
E3..E8) and bring it up. This is the **Secure-Enclave (SES) provisioning**
path — the Alif-native way an image gets into MRAM — written from a real E8
(`E1M-AEN801`) bench bring-up, including the wiring traps that cost us hours.

> Peer docs: [`bring-up-aen.md`](bring-up-aen.md) (the per-subsystem bench
> runbook); [`aen-se-services.md`](aen-se-services.md) (the runtime
> `se_service_*` API — device/LCS/power queries + the gated DVFS / STOC-update
> path). This guide is specifically the **SES → MRAM → boot** flow.

## 0. The model (read this first)

An Alif Ensemble SoM ships with Alif's **factory Secure Enclave firmware
(SEROM + SES / "System TOC", STOC)** already provisioned — you cannot change
it, and you don't need to. What it does **not** ship with is *your*
application: a fresh SoM reports **`No ATOC`** (no Application Table-of-Contents)
and the M55 cores are held until an app is provisioned.

So "flashing" an Ensemble is not `west flash` to an address — it's:

1. The host talks to the **SES over the SE-UART** (a dedicated maintenance
   UART, *not* the application console).
2. You build an **ATOC** (your app + metadata) with Alif's **SETOOLS**
   (`app-gen-toc`) and write it to MRAM (`app-write-mram`).
3. On the next boot the SES validates the ATOC and launches your M55 image.

Two host paths put an ATOC into MRAM (both go through the SES boot ROM ISP):

- **Flow A — SETOOLS over the SE-UART** (the original path, detailed below).
- **Flow D — J-Link DIRECT MRAM flash over SWD** (now the default burn path on
  the bench). J-Link's built-in Alif MRAM loader activates when you select the
  **part-number device profile** (`AE822FA0E5597LS0_M55_HE`), *not* the generic
  `Cortex-M55`. It burns the signed ATOC over SWD in ~0.16 s, verifies, then
  re-runs the SE boot ROM (reset via the nRESET pin) so the app boots from
  MRAM. Helper: `bench-builds/flash-jlink.sh`. Needs the J-Link V9.46+ DLL
  (bench has V9.50) and a probe on matched J-Link V13 firmware (May 2026).

> The earlier blanket claim that *J-Link cannot write MRAM on this part* was
> only ever true for the **generic** `Cortex-M55` profile — with the part
> profile selected, J-Link's MRAM loader does burn Alif MRAM.

What still requires the SES first is *debug-AP* access on a **truly blank**
board: until the SES releases the core, SWD reaches the SoC's SW-DP but the
core's debug-AP is gated (`Could not find core in CoreSight setup`). The Flow D
MRAM loader works through the SES boot ROM ISP, so it does not depend on the
core being released.

## 0.5 If your module came from Alp Lab — you probably don't need this

**E1M-AEN modules ship pre-provisioned by Alp Lab.** At manufacturing we write
a development-signed **MCUboot** bootloader as the factory ATOC and a small
**self-test** image into MCUboot's primary slot (slot0), with the module left
in lifecycle state **DM** (development — debug open, fully re-provisionable).
So out of the box your module:

- **boots on its own** (the self-test runs — proves the unit at our QA), and
- the M55 core is **already released**, so **`west flash` / SWD just work**.

That means your day-1 path is the normal Zephyr one — no SETOOLS, no SE-UART:

```bash
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <your-app> \
    --sysbuild -- -DSB_CONF_FILE=<abs-alp-sdk>/zephyr/sysbuild/aen/sysbuild.conf
west flash    # writes your MCUboot-signed image into slot0 over SWD
```

You only need the SES/SE-UART flow in this guide if you are:

1. **re-keying** to your own production signing key (replacing Alp's dev
   MCUboot — see [`secure-boot.md`](secure-boot.md)), or
2. **recovering** a module whose ATOC was wiped/corrupted (back to `No ATOC`),
   or bringing up a **bare module** sourced outside Alp Lab.

The rest of this document is that path.

## 1. What you need

* **Alif Security Toolkit (SETOOLS)** — Alif Developer download
  (`app-release-exec-linux-SE_FW_x.y.z`). Contains `tools-config`,
  `maintenance`, `app-gen-toc`, `app-write-mram`, the stock `m55_blink_*`
  examples, and the user guide PDF.
* **A 1.8 V-capable USB-UART** for the SE-UART (see §2 — this is the #1 trap).
* The board powered (1 A bench supply is plenty; a fresh SoM idles ~80-150 mA).
* A **SWD/J-Link probe** — for **Flow D** it is the burn path itself (select
  the part-number device profile so the built-in Alif MRAM loader activates),
  and it confirms the core came alive after provisioning. Needs the J-Link
  V9.46+ DLL and a probe on matched J-Link V13 firmware. *Optional* if you only
  use the SETOOLS/SE-UART path (Flow A).

## 2. Wire the SE-UART — the part everyone gets wrong

The SES maintenance UART (**SEUART**) is **not** the application console. On
E1M-AEN modules it is a dedicated, reserved service pair exposed by the carrier
as `SEUART_TX` / `SEUART_RX`. The documented edge UARTs (`UART0`, `UART1`) are
the **application** console — connecting there, the SES never hears you.

Wire it **crossed**, and mind every one of these — each was a real failure
mode on the bench:

| Must-do | Why (failure mode if wrong) |
|---|---|
| **1.8 V logic level** (adapter VCCIO = 1.8 V, *not* 3.3 V/5 V) | The SoM IO is 1.8 V. A 3.3 V FT232's RX threshold (~2.0 V) won't register a 1.8 V HIGH → you'll see the signal on a scope but the UART decodes **nothing**. Also protects the SoM's non-3.3 V-tolerant RX pin. |
| **Crossed** TX/RX: adapter **TXD → SEUART_RX**, adapter **RXD ← SEUART_TX** | Straight-through = no comms either way. "Both wires connected" ≠ "crossed". |
| **Common GND** (adapter GND ↔ SoM GND) | The classic "scope sees a clean signal but the UART gets 0 bytes" cause — no shared reference, no framing. |
| **Right service pins** (`SEUART_TX` / `SEUART_RX`, *not* `UART0`/`UART1`) | Wrong pins = you're on the app console; the SES is silent there. |
| **Baud = 57600** (E8/E6/E4) or **55000** (E7/E5/E3/E1) | Wrong baud → "Target did not respond". |

**Sanity-check the adapter before blaming the board:** jumper the adapter's
own TXD↔RXD and loop bytes through it — it must echo. (A dead-RX adapter
loops back fine via its internal ground but never hears the board; swap it.)

**Confirm the link** by listening for the boot banner (replace the port):

```bash
python3 - <<'PY'
import serial, time
# <your-serial-device>: your OS's port name for the SE-UART adapter --
# see docs/cross-platform-setup.md §7.7 for the per-OS naming convention.
s = serial.Serial('<your-serial-device>', 57600, timeout=1)   # 55000 for E7/E5/E3/E1
buf=b""; t0=time.time()
while time.time()-t0 < 30: buf += s.read(4096)
print(len(buf), "bytes"); print(buf.decode('ascii','replace'))
PY
```

Power-cycle the board during that window. You should see:

```
SEROM v1.x.y
SES Ax v1.x.y ...
[SES] No ATOC          <-- fresh board: no app yet
[SES] STOC DEVICE ok   <-- factory SES present + healthy
[SES] M55-HE booted from address 0x58000000
[SES] LCS=1            <-- lifecycle: 1 = DM (dev), provisionable
```

(Use a single-session reader as above — *not* `stty` + `cat`, which on USB
serial can drop the baud between opens.)

## 3. Configure SETOOLS for your part

From the SETOOLS dir, auto-detect over the SE-UART (`<your-serial-device>`
is your OS's port name for the adapter — see docs/cross-platform-setup.md
§7.7 for the per-OS naming convention):

```bash
./tools-config -a -c <your-serial-device> -b 57600
```

It probes the SES and reports e.g. `Target part# AE822FA0E5597LS0 matches
default E8`. If it can't reach the SES, fix §2 first (auto-detect needs the
**send** direction working too, not just receive).

## 4. Build the ATOC + write it

Use the stock blink first to validate the path end-to-end before your own
image. The on-module factory **DEVICE** config is already correct for your
part, so write an **app-only** ATOC (don't overwrite the device config —
a mismatched DEVICE config is the documented crash cause):

`build/config/app-blink-only.json`:
```json
{
  "BLINK-HE": {
    "binary": "m55_blink_he.bin", "version": "1.0.0", "signed": true,
    "cpu_id": "M55_HE", "loadAddress": "0x58000000", "flags": ["load", "boot"]
  }
}
```

```bash
./app-gen-toc -f build/config/app-blink-only.json     # builds build/AppTocPackage.bin (tagged with the tools-config part)
./app-write-mram -c <your-serial-device> -b 57600     # resets to maintenance, writes the ATOC
```

`app-write-mram` resets the target into **Maintenance mode** (cores held)
and writes. If it sits at `Waiting for Target..[RESET Platform]` it's in
**Hard-maintenance** — **power-cycle the board** and it catches the SES in
its boot ISP window. A clean write ends with `100% ... Done`.

> Your own app: build the Zephyr image for
> `alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` (or `/rtss_hp`), point the
> ATOC `binary` at its `.bin`, keep `loadAddress 0x58000000` for HE.

## 5. Confirm boot

Power-cycle and re-run the §2 listener. The banner should now show the ATOC
present and your image booting (instead of `No ATOC`). Once the SES releases
the core, **J-Link can attach** for normal SWD debug — use the **generic
`Cortex-M55` device**, not the Alif part number (the part-specific device
connect sequence fails post-boot; the generic core device finds the released
core at AP[3]):

```bash
JLinkExe -device Cortex-M55 -if SWD -speed 4000 -nogui 1
```

On the E1M-AEN801 this reads SW-DP IDR `0x4C013477` and CPUID `0x411FD220`
(Cortex-M55 r1p0), with `Secure debug: enabled` — i.e. the SES released the
core and full SWD debug is now available.

## 6. Troubleshooting (from the bench)

| Symptom | Cause / fix |
|---|---|
| Scope shows `SEROM v1…` but the host reads **0 bytes** | No common **GND**, **3.3 V adapter** on a 1.8 V line, or you're on the **app UART** not the SEUART. |
| `Target did not respond` (no scope signal either) | Wrong **baud** (57600 vs 55000), or the SES isn't in its ISP window — use **Hard-maintenance** (`maintenance` → Device Control → Hard maintenance mode) and power-cycle. |
| Adapter loopback (TXD↔RXD jumper) echoes nothing | Dead/incompatible adapter — swap it (and ensure 1.8 V VCCIO). |
| `app-write-mram` warns "device in SEROM Recovery mode" | No valid SES — recover the SES first (`maintenance` recovery), it's not a normal app-write. |
| Image written but won't boot | ATOC built with the wrong **DEVICE** config for the part — re-run `tools-config` for the correct part and rebuild the ATOC (or write app-only, keeping the factory DEVICE config). |
| J-Link `Could not find core in CoreSight setup` | Normal on a **fresh** board — the SES hasn't released the core. Provision an app first. |
| J-Link hangs on a firmware update on first connect (Flow D) | A version-mismatched probe forces a J-Link firmware update that **times out over a USB hub** — connect the probe to a **direct root USB port**. |

## See also

- [`bring-up-aen.md`](bring-up-aen.md) — per-subsystem bench runbook (rails,
  SWD, EEPROM, peripherals, NPU, model load).
- Alif **Security Toolkit User Guide** (ships in the SETOOLS package) — the
  authoritative reference for `tools-config` / `app-gen-toc` / `app-write-mram`,
  lifecycle states, and recovery.
