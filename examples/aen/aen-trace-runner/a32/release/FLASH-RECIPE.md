# HP vision release: exact flash recipe for the bench-runner

**Fix round 1 (reviewer): the previous version of this file wrote exact images (whole-16-KiB-sector
rewrites fill everything outside the image with 0xFF, alp-sdk#2233), never suppressed `loadbin`'s
implicit reset, and never proved the write landed. Fixed by moving to `a32/release/
flash-release.sh`, which delegates sector-padding, the noreset write, and a fresh-session
read-back proof to the SAME proven Flow D machinery `probe/npu/flash-probe.sh` already uses
(alp-sdk's `scripts/bench/aen/bench-env.sh`) instead of hand-rolled J-Link text.**

**Fix round 2 (reviewer, see the implementor report): round 1's write session still had a RESET
before the halt (`RSetType 2; r; h`) -- that reset was measured (alp-sdk-lcd
`scripts/bench/aen/flash-jlink-mramxip.sh:296-321`) to DESTROY the debug access the halt needs on
this part: 0 of 8 halts succeeded after a reset in that bench log, versus 12/12 with a plain `h` on
the live core. **The correct v5.2 session is `connect` -> `exec SetSkipProgOnCRCMatch = 0` -> `h`
(NO prior reset) -> the loadbins, each `, noreset` -> `RSetType 2; r; g` ONLY AT THE END**, and the
halt itself is confirmed by grepping the transcript for a `^PC = ........, CycleCnt = ` line
BEFORE the first `Downloading file` (`flash-jlink-mramxip.sh:420-446`) -- no such line means the
core never actually halted and NOTHING was written (`noreset` leaves no fallback). Also fixed this
round: the ATOC guard's allow-list (`BOOTLOAD A32_APP HP_APP HE_APP`, the real `gettoc` row names,
not a made-up `HP-APP`), the skip-if-unchanged check now applies to every blob, not just `bl32`,
and the missing `LG_SWD_PATH`/`SETOOLS_DIR` exports below.**

**Fix round 3 (small follow-ups, FLASH-READY per the final re-review): `flash-release.sh write`
now REFUSES a changed `bl32` outright (this flow only updates `hp_vision`/the model, never the
bootloader) instead of writing it; the halt-fail path now runs the SAME fresh-session proof +
race check before claiming "NOTHING was written", so a write that somehow landed despite the halt
report is caught, not assumed away; `write` also refuses a package whose `recipe.txt` has no
`movenet_model` item (not a `TR_HP_VISION=ON` build). `src/platform/a32.c` now clears
`TR_MEM_SRAM1_READY` in an early `SYS_INIT` (so a warm reset does not leave a PREVIOUS boot's
ready magic sitting there) and on the `sram1_answers()` timeout path too. Step 2 below now names
the exact frozen artifacts the first flash uses.**

**Fix round 4 (post-flash silicon finding): the first flash's own HP ram console caught a real
arena-sizing bug -- `alp_inference_open` failed with `Failed to resize buffer. Requested: 284288,
available 283864, missing: 424.` TFLM's persistent allocations sit on top of Vela's 277.5 KiB
figure, which was never the whole arena requirement. `TR_MEM_NPU_ARENA_SIZE` grown to 286,720 B
(`src/ipc/tr_memmap.h`), `TR_DL_MAX_TRIS` trimmed further (`src/render/r3d.h`, 4528 -> 4489) to
make room -- see both files' own comments. Also merged `feat/power-hud` (the "+5V ... mW SoM+LCD"
HUD line). The board currently holds the FIRST NPU release (the one that hit this bug), not v5.2
-- `bl32` is unchanged from it, everything else in the blob list below is not.**

**Regenerated fix round 13 (review: AE-units correction + group hold in hp_vision,
render.c:525 bounds fix, font3x5 'N' fix, rail5v sanity gate, hp_vision_check.sh
scanner fix -- this table dated from fix round 12) against commit head at the time,
`a32/release/build-release.sh` run with no read-back argument. `bl32`'s md5 is STILL
UNCHANGED (the board already holds it); `movenet_model`'s is unchanged too (same
Vela'd cut model, untouched by this round). `a32_app` (the font3x5['N'] fix, its
only renderer-payload change this round) and `atoc` DID change -- HP_APP's own
AE/group-hold/readback changes ship inside `atoc` too (the packaged HP_APP image is
part of the ATOC, not a flowd/ item of its own; hp_vision's own build re-links every
round regardless of whether its object code moved, so `atoc`'s md5 is expected to
change even on a round that touched HP_APP alone). Confirm the board's actual MRAM
state with a fresh read-back before trusting any md5 here against live hardware.

**Regenerated fix round 14 (silicon REGRESSION on the round-13 image: the group
hold froze the camera stream at frame ~30, first apply_ae() call -- see
hp_vision/src/main.c's own comment. Dropped the group hold entirely, no vendor-
driver or datasheet evidence it was even the right register; re-verified the
readback always runs; added an HP-side >500 ms stall guard; redrew font3x5['N']
again, both edges now solid on every row; rail5v/render.c:525 fixes from round 13
stand unchanged). `bl32`/`movenet_model` unchanged again. `a32_app` changed (the
second 'N' redraw); `atoc` changed (HP_APP's group-hold removal + stall guard).
Confirm against a fresh read-back before trusting these against live hardware --
round 13's own image was flash-ready by every local gate and still froze the
camera on first silicon boot.

**Camera orientation (npu-body review round): `TR_CAM_ROTATE` now defaults to `90`
(`src/vision/cam_rot.h`), the bench-verified 2026W36-0009 mount -- `270` showed the player upside
down, the maintainer confirmed `90` by eye. Build the HP image with it spelled out anyway:
`-DTR_CAM_ROTATE=90 -DTR_CAM_MIRROR=ON`. `build-release.sh` (`hp_vision_check.sh`) prints
`TR_CAM_ROTATE=... TR_CAM_MIRROR=...` from the HP build's `CMakeCache.txt` and REFUSES any
rotation but `0`/`90`/`270` -- including an EMPTY one, which only means "cam_rot.h's default
when it was built" (`270` before this round). An HP build dir from before the option existed
(e.g. `/tmp/tr-hp-vision-build10`, no `TR_CAM_ROTATE` entry at all) is refused for the same
reason: rebuild it. Check the printed line before flashing: `TR_CAM_ROTATE=90
TR_CAM_MIRROR=ON` for 2026W36-0009.**

## Final blob list (real model, this release)

`sha256 6099cdcdff295e25a59107a5318df92f79cfc58ea8c366f5a806f53cf753e898` (the design doc's cut
model) Vela'd exactly as the design doc's table (`ethos-u55-256`, `RTSS_HP_SRAM_MRAM`,
`Shared_Sram`, 400 MHz, `--optimise Size`; Vela's own log confirms 277.50 KiB SRAM, 119/0 NPU/CPU
ops -- matches the design table verbatim), packaged with `a32/release/build-release.sh
TR_HP_VISION=ON` against a real `hp_vision` build and the `TR_INPUT_NPU=ON` HE build:

| item | address | size (B) | md5 |
|---|---|---|---|
| bl32 | `0x80002000` | 28,816 | `766122d9cebb80bde9a0e346b8a806a2` |
| a32_app | `0x80020000` | 460,800 | `8f7027fd4dd278740fbfa536a5364143` |
| atoc | `0x8051BF90` | 409,712 | `26daa777886b2c13d9f35c7490f3ca75` |
| movenet_model | `0x80100000` | 2,429,520 | `51f3fac2d27a8e2048bc77e0b8010815` |

**Regenerated fix round 12 (review: this table dated from fix round 4) against commit head at the
time, `a32/release/build-release.sh` run with no read-back argument, printing `flowd/recipe.txt`
directly -- `bl32`'s md5 is STILL UNCHANGED across every release so far (the board already holds
it, so `flash-release.sh write`'s skip-if-identical check is expected to skip it, not write it);
`movenet_model`'s is unchanged too (same Vela'd cut model, unaffected by this round's fixes).
`a32_app`/`atoc` DID change (round 12's own fixes -- the MMU NC-page split, the completed font,
the AE register readback fields all changed what the renderer/stub image actually contains, and
by extension the ATOC's own size and address) -- confirm the board's actual MRAM state with a
fresh read-back before trusting any md5 here against live hardware; these are the freshly-built
images' own, not a read-back-merged flash image's.**

**These md5s are of the EXACT images (build-release.sh run without a read-back argument).** The
sector-merged blobs `flash-release.sh write` actually flashes are different files (each padded
out to whole 16 KiB sectors with the live read-back's own neighbouring bytes) and will have
different md5s each run, by design -- `flash-release.sh`'s own fresh-session proof checks the
padded images it built, not these four. `atoc`'s address is computed per-release by `app-gen-toc`
from the package size -- do not reuse `0x8051BF00` for a future release without re-reading
`flowd/recipe.txt`. `build-release.sh`'s own all-pairs sector-overlap assert (`0x4000`-aligned)
proves these four never share a 16 KiB sector.

## Preconditions

Export ALL THREE before anything else in this recipe -- `bench_jlink_run` (`flash-release.sh`'s
only way to reach the probe) refuses outright without `LG_SWD_PATH`:

```sh
export LG_SWD_PATH=<usb-path>   # the J-Link's USB topology path, e.g. from `labgrid-client show`
export SETOOLS_DIR=<dir>        # a PRIVATE `cp -a` of Alif's SETOOLS (app-release-exec-linux);
                                # app-gen-toc rewrites its build/ tree
export SE_UART=<your-serial-device>   # the board's SE-UART, for the ATOC guard
```

## The exact SETOOLS recovery / pre-write gate command

**Run this BEFORE any write in this recipe (original or recovery), and require it to succeed.**
It is what `flash-release.sh`'s ATOC guard (`bench_flowd_atoc_guard`, via `bench-env.sh`) runs
automatically whenever `SE_UART` is exported -- spelled out here so the bench-runner can run it
BY HAND first and see the resident TOC before trusting the script:

```sh
cd <PRIVATE-SETOOLS-COPY> && ./maintenance -b 57600 -c <your-serial-device> -opt gettoc
```

If `gettoc` fails, STOP -- do not attempt any write, original or recovery, until it succeeds (that
is the entire safety property this step buys: proof the SE-UART link can enumerate what is
currently resident before anything overwrites it).

**The SE-UART recovery command itself** -- for when a bad ATOC blocks the SWD debugger entirely
(`flash-release.sh restore`'s J-Link path needs a debugger connection to work at all, which is
exactly the thing a bad ATOC can take away; SE-UART is the recovery path THAT does not depend on
debugger access surviving the bad write):

```sh
# cut the slice first (from the PRE-write read-back, over the exact range you are restoring --
# e.g. the ATOC range, or a whole flowd/ item):
dd if="$RB_PRE" of=/tmp/recovery-slice.bin bs=1 skip=<OFFSET> count=<SIZE>

cd "$SETOOLS_DIR" && ./app-write-mram -b 57600 -c <your-serial-device> \
  -i "/tmp/recovery-slice.bin <ADDR>"
```

**UNVERIFIED: whether the Secure Enclave accepts an arbitrary `<ADDR>` range through `-i` this
way has not been proven on this bench** -- every real SETOOLS write this codebase's own bench
scripts record is either a whole-ATOC `-p` (`app-write-mram -c <uart> -p .`) or an `-e` erase of a
specific, preset-derived window (`erase-storage.sh`); `-i` at an arbitrary address is the
coordinator's own candidate for this recipe, not something this repo has exercised. Confirm it
against the actual `app-write-mram --help` output and a real recovery attempt before relying on
it as the ONLY recovery path a bad ATOC leaves.

## Ordered procedure

**Every step below is ordered. Do not reorder or skip the safety check or the read-back.**

### 1. Live MRAM read-back (rollback basis -- do this before ANY write)

**Name this file explicitly and reuse the SAME name in every step below -- do not rely on a glob
like `mram-readback-*.bin` once a second (post-cycle) read-back exists in the same directory; a
glob then matches both and every command that consumes "the" read-back breaks or picks the wrong
one silently (round 2 finding).**

```sh
RB_PRE=mram-readback-pre-$(date +%Y%m%d-%H%M%S).bin
bash a32/release/flash-release.sh readback "$RB_PRE"
```

Covers `0x80000000..0x8057FFFF` (`build-release.sh`'s own `END - BASE`) -- every sector this
release or a future restore could touch, including person_detect's slot0 window (see the note at
the end). Keep this file until the unit's next confirmed-good read-back supersedes it -- it is the
ONLY rollback basis.

### 2. Rebuild with the read-back as the sector-merge basis

**The first flash uses the FROZEN, already-reviewed artifacts below -- do not rebuild into, modify
or delete them** (but see "Camera orientation" above: `hp_vision_check.sh` now refuses an HP build
without an explicit `TR_CAM_ROTATE`, so a pre-rotation HP dir like `build10` needs replacing by a
build with `-DTR_CAM_ROTATE=90 -DTR_CAM_MIRROR=ON`):

```sh
TR_HP_VISION_BUILD=/tmp/tr-hp-vision-build10
TR_HP_VISION_MODEL=/tmp/tr-npu-vela/movenet_cut_u55-256/movenet_cut_vela.tflite
HE_BUILD=/tmp/tr-he-npu-build5   # TR_INPUT_NPU=ON

# SETOOLS_COPY must be a FRESH cp -a of $SETOOLS_DIR for this run, not that dir itself, and
# never reused across runs -- build-release.sh + app-gen-toc rewrite build/ in place, and a
# reused copy from a previous run/session would mix this release's config into whatever earlier
# state that copy was left in.
ST=$(mktemp -d)/setools && cp -a "$SETOOLS_DIR" "$ST"

TR_HP_VISION=ON TR_HP_VISION_BUILD="$TR_HP_VISION_BUILD" TR_HP_VISION_MODEL="$TR_HP_VISION_MODEL" \
  bash a32/release/build-release.sh "$ST" "$HE_BUILD" "$RB_PRE"
```

The third argument is what fixes the "neighbouring memory gets wiped" blocker: `$ST/build/flowd/`
now contains WHOLE-16-KiB-SECTOR blobs (bytes outside each image are the live read-back's own
bytes, not `0xFF`) instead of exact images. `flash-release.sh write` reads these (and now refuses
outright if `$ST/build/flowd/recipe.txt` has no `movenet_model` item -- a package not built with
`TR_HP_VISION=ON`), not the exact per-item files. Use `"$ST"` (this run's fresh copy), not
`"$SETOOLS_DIR"`, in step 4 below.

### 3. A32-not-executing-from-MRAM safety check

```sh
bash a32/release/flash-release.sh check-a32
```

Reads `0x02401184` (SRAM1 mailbox `TR_MBOX_ADDR` `0x02401000` + `0x184`, `ctrl_entry` -- the
offset-`0x180` stub-control block's second word). **Proceeds only if the read is exactly
`0x02500000`** (the renderer's SRAM1 entry) -- the last LAUNCH the stub issued, i.e. the A32 is
executing from SRAM1, not MRAM. The script `die()`s on any other value; if that happens, halt the
A32 core via the stub's HALT control word first (`a32/renderer/halt.jlink`) and re-check before
going any further. **Sanity-check the script's own parse of the `mem32` output by eye the first
time this runs on real hardware** -- this repo has never run this exact text-parsing against a
real J-Link session's output format; do not trust it blind on a first bench use.

### 4. Write

```sh
bash a32/release/flash-release.sh write "$ST" "$RB_PRE"
```

Session: `connect` -> `exec SetSkipProgOnCRCMatch = 0` -> `h` (halt the LIVE core, NO reset
first) -> the pre-write race-check `savebin`s -> every sector-merged blob's `loadbin ..., noreset`
-> `RSetType 2; r; g` (the ONE reset in the whole sequence, booting the new image, only after
every write). Every blob that is byte-identical to the live read-back at its address is skipped
automatically (one fewer sector touched for no reason, and one fewer sector this session's own
halt/noreset risk touches) -- this applies to ALL FOUR items, not just `bl32`. The script itself
greps the transcript for the halt proof (a `^PC = ........, CycleCnt = ` line before the first
`Downloading file`) and refuses (exit 5, nothing written) if the core never actually halted.

### 5. Proof-of-write

Built into the same `write` invocation above (a FRESH J-Link session, a new process, so nothing
can be served from another process's flash cache): `savebin`s every written range back and `cmp`s
byte-for-byte against the padded images actually flashed, plus a pre-read/write race check
(did anything else touch a to-be-padded sector between the plan's read and the load). Any
failure exits non-zero with `RACE` or `READ-BACK PROOF FAILED` and explicit instructions not to
trust the board. **This is not a cold-cycle persistence proof** (see `bench_flowd_proof`'s own
header note: a documented case exists where `Verify successful.` was followed by a cold-cycle
revert) -- step 6 is what actually proves persistence.

### 6. Cold cycle, then repeat the read-back compare

Power-cycle the unit (full cold boot, not a debugger reset). Then take a SEPARATELY-named
read-back and verify it against each `flowd/` blob directly (NOT against `$RB_PRE`, which is the
pre-write capture and by definition differs in every range this release just wrote; NOT a
whole-file `cmp` either -- unwritten MRAM, e.g. person_detect's slot0, see below, is expected to
still read as `$RB_PRE`, and a whole-file compare would misreport that as a failure here):

```sh
RB_POST=mram-readback-postcycle-$(date +%Y%m%d-%H%M%S).bin
bash a32/release/flash-release.sh readback "$RB_POST"

# One dd slice per flowd/*.bin, compared against that exact range of $RB_POST:
for f in "$ST"/build/flowd/*.bin; do
	[ -e "$f" ] || continue
	name=$(basename "$f" .bin)
	addr="0x${name##*-0x}"
	sz=$(stat -c %s "$f")
	off=$(( (16#${addr#0x}) - 0x80000000 ))
	slice=$(mktemp)
	dd if="$RB_POST" bs=1 skip="$off" count="$sz" status=none > "$slice"
	if cmp -s "$slice" "$f"; then
		echo "PERSISTS: $name ($addr, $sz B) -- md5 $(md5sum <"$slice" | cut -d' ' -f1)"
	else
		echo "!! DID NOT PERSIST: $name ($addr, $sz B) -- cold cycle reverted this blob" >&2
	fi
	rm -f "$slice"
done
```

### 7. Three cold boots

Repeat step 6's power cycle three times total, confirming each time (via the HP debug beacon and
pose slot over SWD, and the live TOC) that the image is still resident and boots the same way.
One successful cold cycle proves persistence for THAT cycle; a booth unit needs to survive many.

## Post-flash verification

Read the HP's debug beacon (`hp_vision`'s `TR_MEM_HP_DBG` = `0x0237FCA0`, magic `0xA11FE000`) and
the pose slot (`TR_MEM_PSLOT` = `0x0237F200`, magic `'TRPS'` = `0x54525053`) over SWD -- a live,
advancing `heartbeat` word at `TR_MEM_HP_DBG+0xC` confirms the HP booted and is looping;
`hp_state == TR_HP_STATE_RUNNING` (0) at the pslot's **`+0x80`** (`tr_pslot.h`'s
`_Static_assert` block is the binding source) confirms the camera+NPU pipeline itself is live,
not just the core. **`sram1_ready_seen`/`sram1_ready_at_heartbeat`** (`TR_MEM_HP_DBG+0x38`/`+0x3C`)
report whether and when `tr_sram1_ready()`'s gate (fix round 2) ever passed this boot -- `0`/`0`
means CAM_POOL (SRAM1) was never confirmed safe and the camera was never opened; watch the HP's
console for the matching `sram1   :` lines too.

Also re-read the live TOC (`gettoc`, the same command as the pre-write gate above) to confirm
`HP_APP` now reports the new `hp_vision` image size, not the old 4480 B stub.

## Recovery

```sh
bash a32/release/flash-release.sh restore "$RB_PRE"
```

Same `gettoc` precondition as above, same halt-once/noreset/proof machinery as `write` -- puts
every one of the four ranges this release ever touches back from a chosen read-back file.

## person_detect's slot0

Trace-runner's release (`bl32`/`a32_app`/`atoc`, and now `movenet_model`) writes into the same
physical MRAM window person_detect's own slot0 occupies on whichever core it targets (HE
`slot0_partition` `0x80010000..0x802AFFFF`, or HP's disjoint window `0x802B0000..0x8054FFFF` --
board dts, `metadata/e1m_modules/E1M-AEN803.yaml`) -- **this has always been true of
a32_app/bl32/atoc**; `movenet_model` (`0x80100000..0x8035124F`) only extends that
already-overlapping footprint a little further (into HE's slot0 tail and the following
unpartitioned gap), it does not newly touch either core's `reserved`/`storage`/own-`atoc`
partitions. Restore with `flash-release.sh restore` (or the `gettoc`-gated SETOOLS command above,
for the ATOC specifically) once bench work with this release is done, and confirm md5
`5839e003d5d069f6fd912eb22774d037`.
