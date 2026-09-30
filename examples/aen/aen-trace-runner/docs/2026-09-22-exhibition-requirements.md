# Trace Runner — exhibition requirements

**Added 2026-09-22** after the decision that this runs as a booth demo, not
only as a bench demo. Touch (GT911) and the HyperRAM R3 fix are **out of scope**
for now; this document assumes neither exists.

An exhibition build has a different definition of "works". On the bench, a
human who understands the system is holding it and can power-cycle. In a booth
it runs for eight hours in front of strangers, nobody watching it can debug it,
and the failure everyone remembers is the one that happened while they were
looking.

## What changes because of the venue

### 1. Attract mode — the biggest missing piece

Right now, with nobody in front of the camera, the game waits. An arcade
cabinet never waits: it plays itself and invites you in. Without this, a
passer-by sees a still screen and keeps walking, which means the demo fails at
the only moment that matters.

Needed: when no player has been detected for a few seconds, the game drives
itself with synthetic input — visibly playing, dodging, collecting — behind an
invitation banner. Any real detection takes over immediately and starts a fresh
run.

This also solves the "player walked away mid-run" case more gracefully than a
pause banner does.

### 2. More than one person in frame

**This is the most likely failure at a booth and the current design gets it
wrong.** `detect.c` takes the bounding rectangle of *all* foreground, so two
people standing side by side produce one box spanning both of them, whose
centre is the empty space between them. The game then tracks a phantom player
in the middle lane while two real people wave at it.

At an exhibition there will almost always be two people. Needed: pick one
subject — the largest connected blob, or the one nearest the frame centre —
rather than unioning everything that moved. That is a real change to the
detector, not a threshold tweak.

### 3. Recovery from everything, without a power cycle

Already handled, and must not regress: the calibration retry, the timeout that
falls back to tilt, the display-open retry.

Still to handle:
- A player who leaves mid-run (attract mode covers this).
- Someone standing far too close, filling the frame — the blow-out guard reads
  this as a lighting change and rejects it, which is correct, but the screen
  must say something rather than appearing frozen.
- The camera being blocked or knocked. `CHECK THE CAMERA` exists; at a booth it
  should also keep attract mode running so the screen is never dead.

**Tilt mode is not a useful exhibition fallback.** The board will be fixed to a
mount and nobody will pick it up. Where the current build falls back to tilt,
an exhibition build should fall back to attract mode instead. Keep tilt for the
bench.
A tilt takeover of camera-less attract exists as a bench/dev option only
(`-DTR_TILT_TAKEOVER=ON`, off by default; see `src/game/tilt.h`).

### 4. Lighting

Booth lighting moves: spotlights, camera flashes, people walking behind the
player, daylight changing through a hall window. The background model adapts
over roughly 14 seconds and the blow-out guard catches whole-frame changes, but
neither was designed against a moving crowd behind the subject.

This needs measurement in the venue rather than more code written blind. The
one cheap insurance policy: make the detector's thresholds adjustable without a
rebuild, so they can be tuned on site in minutes.

### 5. Running for hours

- Check every counter that advances per tick or per run for overflow across an
  eight-hour day, and say what happens at the limit.
- The RAM console wraps with no marker. It is a bench instrument, not an
  exhibition one — nothing about the demo's behaviour may depend on reading it.
- Everything an operator needs must be visible **on the panel**.

### 6. Cold-boot reliability

The panel's init fails on roughly 1 cold boot in 8-10 with no re-init path.
Mitigated by five retries at 200 ms, but those attempts are correlated rather
than independent, so the real improvement is unmeasured.

For a booth this is the difference between a demo that switches on and one that
needs a person. Worth measuring properly — cold-cycle it many times and count —
before relying on it.

### 7. Physical setup

Not firmware, but it decides whether the firmware's assumptions hold:
- Fixed mount, fixed camera aim, known distance.
- A marked spot on the floor where the player should stand, at the distance the
  lane bands were calibrated for.
- Consider marking where NOT to stand, to reduce the two-person case.

## Priority for a booth

1. **Attract mode** — without it the demo does not attract.
2. **One subject, not the union of everyone** — the most likely visible failure.
3. **Cold-boot reliability measured** — it either switches on or it does not.
4. **On-site tunable thresholds** — the venue is not the lab.
5. Long-run counter audit.

## What this decision costs elsewhere

Dropping the HyperRAM fix keeps external memory unavailable, which leaves
roughly 9.75 MB of on-chip RAM for everything, 3.7 MB of it consumed by
double-buffering. That removes most of the reason to bring up the two Cortex-A32
cores for rendering: they would carry the full cost of a new bare-metal port,
an unexercised mailbox and cross-architecture cache coherency, for a memory
budget no better than the M55s already have.

So the core plan simplifies to **M55-HE for I/O and M55-HP for the heavy work**,
which is the bench-proven path anyway. The A32 renderer stays documented in
`2026-09-22-core-allocation.md` as the option that opens up if HyperRAM is ever
fixed.
