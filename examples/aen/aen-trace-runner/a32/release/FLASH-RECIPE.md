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

**Camera orientation: the E1M-EVK bench release is LANDSCAPE, `-DTR_CAM_ROTATE=0 -DTR_CAM_MIRROR=ON`
on BOTH the HE and the HP build.** The arm-raise controls (README "Controls") want the wider field
of view -- arms reach sideways -- so the OV9281 on the E1M-EVK's RPi CSI connector is mounted
upright and shown as a 640x400 picture scaled up x1.28 to cover the camera area (the bottom 2/5 of
the screen, about 10 px cropped a side), with the lamps and the "CAMERA / NPU Hz" label on a plate
along its bottom edge. Mirror ON makes the player see a mirror image of
themselves, which is also what tells the HE which arm is their left (`src/vision/pose.c`). A rig
with the camera mounted on its SIDE uses `90` (the 2026W36-0009 mount; `270` showed the player upside
down, the maintainer confirmed `90` by eye -- `src/vision/cam_rot.h` defaults to it) with
`-DTR_CAM_MIRROR=ON`. **Since Stage 1 the A32 renderer draws only rotation 0.** A rot-90 / 270 build
(the side-mounted 2026W36-0009 rig) still plays -- the HP decodes and the game reads the pose -- but
the video area shows `ROT 90` in its label instead of the camera picture, and draws no skeleton.
Spell the rotation out in every build: `build-release.sh`
(`hp_vision_check.sh`) prints `TR_CAM_ROTATE=... TR_CAM_MIRROR=...` for the HP build and for the HE
from their `CMakeCache.txt`, REFUSES any HP rotation but `0`/`90`/`270` -- including an EMPTY one,
which only means "cam_rot.h's default when it was built" -- and REFUSES an HE/HP pair that disagrees
on `TR_CAM_MIRROR` or on whether the rotation is `0` (landscape and portrait frames differ in size;
an HE built before `TR_CAM_MIRROR` existed has no entry and is refused too: rebuild it). An HP build
dir from before the option existed (e.g. `/tmp/tr-hp-vision-build10`, no `TR_CAM_ROTATE` entry at
all) is refused for the same reason: rebuild it. Check the printed lines before flashing:
`TR_CAM_ROTATE=0 TR_CAM_MIRROR=ON` (HP and HE) for E1M-EVK, `TR_CAM_ROTATE=90 TR_CAM_MIRROR=ON` for
2026W36-0009.

**Bench acceptance: the arm lanes (do this before calling an E1M-EVK release good).** With the game
running and a player in front of the camera:

1. Raise your physical LEFT arm. The figure's raised arm must appear on the screen's LEFT, and the
   runner must move ONE lane left (the "LEFT ARM" lamp lights). Lower it and raise the RIGHT arm: the
   figure's arm on the screen's RIGHT, one lane right ("RIGHT ARM" lamp).
2. Raise both arms together: the runner jumps ("BOTH ARMS" lamp) and does not change lane.
3. If the picture is not mirrored (your left arm shows on the screen's RIGHT), the sensor flip did
   not take: look for `camera  : mirror 1 -> ... -- MIRROR NOT APPLIED` on the HP console.
4. If the picture IS mirrored correctly but the runner moves the OPPOSITE way, or if the picture
   looks mirrored the wrong way round, the rot-0 flip bit (0x3821 bit 2) mirrors the other way to
   the one assumed. The Linux `ov9282` driver has had its hflip inverted against the silicon, and
   nothing here had put a rot-0 picture on glass before this release. Rebuild ONLY the HP image with
   `-DTR_OV9281_HMIRROR_ACTIVE_LOW=ON` (`src/vision/cam_rot.h`): it writes the bit the other way
   round and still reports the truth in the camera descriptor. The HE does not need rebuilding:
   it takes "is the view mirrored" from that descriptor (`tr_cam_view_t.mirror`), not from its own
   `TR_CAM_MIRROR`, and prints `!!!!! HE built for TR_CAM_MIRROR=...` once if the two differ.

## This release reflashes the HE, the HP, the A32 app and the ATOC together

Compared with the previous release this one changes the HE image (mailbox version 4: Stage 1's
panel width `fw` in every frame, on top of version 3's memory re-plan, which moved FB B to
0x025EA000 and the DL, bins, stacks and gate; frames carry a rotation since version 2), the HP vision image (it waits for the HE's I2C1 release before it touches
the bus), `a32_app` (the stub and renderer, now speaking mailbox version 4) and therefore the
ATOC that carries the HE. Flash all of them from ONE build. `bl32` and `movenet_model` are
unchanged (the board already holds them; `flash-release.sh write` skips an identical sector).
Do not mix images across releases:

| Mixed set | What happens |
|---|---|
| new HE, old `a32_app` (stub v2 or v3) | the HE logs `stub speaks mailbox version 3, this HE 4 -- not driving it`; it never treats the stub as alive, so no frames are drawn (a Stage 0 renderer is v3: it cannot crop to `fw`, an 800-wide HE frame would be written 720 wide into a framebuffer scanned 800 wide) |
| old HE (v2 or v3), new `a32_app` (stub v4, new renderer) | the old HE refuses the stub: its mailbox version is not the stub's 4, so no frames are drawn; an HE that ignored the check would scan FB B at 0x02600000 while the renderer draws at 0x025EA000, or never set `fw` (the renderer would fault: `renderer refused fw=0`) |
| new HP, old HE | the HP waits for an I2C1 release that the old HE never publishes: `i2c-handover: waiting ...` on its console, no camera, no pose, the game runs on its fallback |
| old HP, new HE | the old HP touches I2C1 at its boot without waiting; on the RVT121 that collides with the bridge configuration |
| new ATOC, old `a32_app` (or the reverse) | the ATOC's HE and the MRAM renderer disagree on length and CRC: the stub refuses the LAUNCH (`BAD_CRC`) and the HE's watchdog relaunches the same bytes |

`flash-release.sh` writes the images `build-release.sh` packaged together, in one pass; the proof
step reads the same set back. Build the whole release on ONE machine with ONE toolchain (see
`README.md`): `build-release.sh` refuses an HE whose launch header came from a different
`renderer.bin`.

**The table below is PRE-STAGE-1 and historical** (mailbox version 2, 720 wide, `TR_CAM_ROTATE=90`):
none of it matches a current release. A current release's image md5s are the ones its packaging run
prints (`flowd/recipe.txt`, and the `images.md5` the release script writes next to it); compare
those, never this table.

Image md5s of the builds this recipe was last regenerated from (the exact images; the
sector-merged blobs `flash-release.sh write` flashes are padded with the live read-back and have
different md5s each run, by design; the `hp_vision` image's md5 is whatever the packaging run's
`TR_HP_VISION_BUILD` produced and is printed with it). Riverdi RVT121 release, RK055 release (30 Hz via
`panel_30hz.overlay`), both `TR_INPUT_NPU=ON`, `TR_CAM_ROTATE=90`; the renderer, stub and launch
header are the SAME files in both:

| image | md5 |
|---|---|
| `renderer.bin` (460,460 B, `TR_A32_CRC 0xD6605303`) | `47e15a9eecddba4fb21116b7c7b9f7be` |
| `a32_stub.bin` | `c4cd1b52855bfbc329f0bf625df3f3f5` |
| `tr_launch.h` | `282bf49ccf63ead0b651270f96e5b378` |
| RVT121 `he_zephyr.bin` | `69c7e605e77357e8309199aa586d7a86` |
| RK055 `he_zephyr.bin` | `8547ba008af1fa5b9b360b5b649716a5` |

The packaged items' addresses and sizes (`a32_app`, `atoc`, `bl32`, `movenet_model`) come from the
`flowd/recipe.txt` the packaging run prints; this file no longer carries a table of them, because
`atoc`'s address and size follow the package and change with every HE and HP. The release is
packaged with `a32/release/build-release.sh TR_HP_VISION=ON` against a real `hp_vision` build and
the `TR_INPUT_NPU=ON` HE build, the Vela'd cut MoveNet model
(`sha256 6099cdcdff295e25a59107a5318df92f79cfc58ea8c366f5a806f53cf753e898`, `ethos-u55-256`,
`RTSS_HP_SRAM_MRAM`, `Shared_Sram`, 400 MHz, `--optimise Size`; Vela's log confirms 277.50 KiB
SRAM, 119/0 NPU/CPU ops). `build-release.sh`'s own all-pairs sector-overlap assert
(`0x4000`-aligned) proves the items never share a 16 KiB sector.

**Another display (the Riverdi RVT121, `-DSHIELD=e1m_evk_rvt121hvdfwca0`):** the renderer is one
binary for every display (the HE sends the panel's rotation in every frame), so `a32_app`, `bl32`
and `movenet_model` do not change between displays. Only the HE image differs, and with it the
ATOC. The HP image is the same `hp_vision`; it waits for the HE's I2C1 release
(`alp,i2c-handover`), so start the HE first.

## Combined HP image: camera + NPU + game sound (`TR_HP_SOUND`, reworked carriers only)

The HP's one image slot can carry `hp_vision` and the game sound together (design and protocol:
`docs/2026-09-23-sound.md`, "Sound with the vision HP"). **Reworked-U46 carriers only: unit
`2026W36-0002` is allowed, `2026W36-0009` is denied** (`sound-carriers.txt`; `build-release.sh`
refuses the denied unit before it builds anything). **Not run on the bench yet: this recipe only
packages and checks; the bench run below is still to be done.**

```sh
# HP: hp_vision with the sound linked in (needs the clockctrl-patched ZEPHYR_BASE)
west build -b alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp -d "$HP_BUILD" hp_vision -- \
  -DTR_CAM_ROTATE=90 -DTR_CAM_MIRROR=ON -DTR_SND_REWORKED_U46=ON -DTR_HP_SOUND=ON
# HE: the Riverdi build, with the HE's side of the I2C2 + GPIO5 lease
west build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he -d "$HE_BUILD" . -- \
  -DSHIELD=e1m_evk_rvt121hvdfwca0 -DTR_M55_AUTOLAUNCH=ON -DTR_A32_LAUNCH_H="$LAUNCH_H" \
  -DTR_INPUT_NPU=ON -DTR_HP_SOUND=ON -DTR_CAM_ROTATE=90
ST=$(mktemp -d)/setools && cp -a "$SETOOLS_DIR" "$ST"
TR_HP_VISION=ON TR_SND_HP=ON TR_SND_CARRIER_SERIAL=2026W36-0002 \
  TR_HP_VISION_BUILD="$HP_BUILD" TR_HP_VISION_MODEL="$MODEL" \
  bash a32/release/build-release.sh "$ST" "$HE_BUILD" "$RB_PRE"
```

`build-release.sh` runs `hp_vision_check.sh` (combined) AND `snd_hp_check.sh` (combined) on that one
HP build, and refuses: a build without `-DTR_SND_REWORKED_U46=ON` or `-DTR_HP_SOUND=ON`; a
`TR_SND_HP_BUILD` that is a different directory (two HP images); a sound-carrying `hp_vision` with
`TR_HP_VISION=ON` alone, or the combined directory as `TR_SND_HP_BUILD` alone; the sound buffers,
heap or thread stack outside the HP DTCM; an HP image over 256 KiB; and an HE image without
`tr_bus2_he_frame` (an HE built without `-DTR_HP_SOUND=ON` never leases the bus: the HP would wait
for ever); an HE whose `TR_CAM_ROTATE` shape (landscape 0 / portrait 90|270) or `TR_CAM_MIRROR`
differs from the HP's, or that was built without `TR_INPUT_NPU` (`hp_vision_check HP MODEL NM HE_DIR
[MODE]`: the HE dir is argument 4); and an HP carrying the DEV underrun positive control
(`-DTR_SND_UNDERRUN_TEST=ON`, by cache entry or by its console text in the ELF). The RK055 shield cannot be the HE of a combined release: it uses GPIO5 for its backlight,
and the HE build refuses `-DTR_HP_SOUND=ON` with it.

What flashes does not change: `bl32`, `a32_app`, `atoc` (which carries the HE and the HP image) and
`movenet_model` are the same four items, and `flash-release.sh write` / `restore` flash them the same
way, with one difference at the end of the session: **`write` does not issue the warm pin reset
(`RSetType 2; r; g`) into a package whose HP image carries the game sound** (detected from the packaged
HP image; force either way with `--no-reset` / `--reset`). A TR_HP_SOUND image must COLD-boot (both cores
come up together, the lease record starts clean); after the write, power-cycle the board (step 6) and do
not resume the old image.
Flash the HE and the HP from ONE build: the pairings that matter are in the table above, plus
`new HP (TR_HP_SOUND) + HE without TR_HP_SOUND`: the HP prints `waiting for the HE's I2C2 + GPIO5
offer` for ever, vision runs, no sound.

**Do not halt the HP (debugger halt, breakpoint, J-Link `h`) during the amp bring-up.** While the HP holds the
bus it beats `hp_beat`; a halted HP stops beating, and after 2 s the HE reclaims the lease (controller stopped, SCL
bus-clear) under a core that is only paused, which then resumes into a bus it no longer owns (its next entry is
refused and the bring-up aborts and starts over). Read the lease record (`0x0237FD40`) and the console without
halting, or halt only when the HP waits for the offer or streams.

Order is free (the lease does not need the HE first; the I2C1 handover still wants both reset
together, as before). Nothing waits on the sound: the camera, the NPU and the pose slot run whether
or not the amps come up.

**The console symbols move per build.** Read `ram_console_buf` from each build's own ELF
(`arm-zephyr-eabi-nm zephyr.elf | grep ram_console_buf`), never from a remembered address: the 26b8855cd
build had the HE's at `0x20006550` (global `0x58806550`) and the HP's at `0x2002CE92`.

The HE console says what the lease did from the evidence: `I2C2 offered to the HP`, `I2C2 leased by the HP`,
`I2C2 back on the HE`; when the whole lease passes between two HE frames the claim is never observed, and the
line reads `I2C2 leased by the HP and returned within one frame: back on the HE`; an unclaimed offer reads
`I2C2 offer withdrawn (not claimed): back on the HE`. Each line ends with `hp_acq=` and `he_regains=`.

**Bench checks to run (not done):**

1. HP RAM console (`ram_console_buf`): `[snd] waiting for the HE's I2C2 + GPIO5 offer`, then
   `HE offered I2C2 + GPIO5: I2C2 device_init -> 0`, steps `1` to `11` with `5b amp 0x4d ACK` and
   `5b amp 0x4e ACK`, `I2C2 + GPIO5 given back to the HE`, `game sound running at 16000 Hz`. HE
   console: `bus2    : I2C2 offered to the HP`, `leased by the HP`, `back on the HE`; the HUD
   `5V -- mW` for the length of the bring-up (a couple of seconds), then a number, and it keeps
   updating.
2. The lease record over SWD at `0x0237FD40`: `+0x00` `he_state` `0x42320000`, `+0x10` `hp_state`
   `0x42320004` (returned), `+0x0C` `he_regains` 1, `+0x28` `hp_acq` 1, `+0x1C` `hp_aborts` 0 (each core zeroes the
   counters it owns at its own boot, `tr_bus2_he_boot` / `tr_bus2_hp_boot`: `he_regains`, `he_reclaims` by the
   HE; `hp_aborts`, `hp_i2s_fu`, `hp_i2s_err`, `hp_acq` by the HP; a cold SRAM0 no longer shows power-up garbage),
   `+0x08` `he_beat` advancing every frame, `+0x20` `hp_i2s_fu` 0 and `+0x24` `hp_i2s_err` 0 while
   streaming. `0x0237FC94` (the I2C1 handover) is untouched by it.
3. Sound plays beside the camera: the pose slot's `hp_state` stays 0, the HP debug beacon's
   heartbeat (`0x0237FCAC`) keeps its frame rate, `hp_i2s_fu` stays 0 for 60 s.
4. Reset the HP alone: its `PRE_KERNEL_1` forgets the lease, the HE (idle, it owns the bus)
   sees the new `WANT`, offers a new token, and the bring-up runs again. An HE-only reset still
   reconfigures I2C1 under the running camera exactly as before (`alp,i2c-handover`): reset both.
   (An HE reset inside the HP's bring-up is covered by the host tests, not by a bench step.)
5. Volume (record `TR_MEM_VOL` = `0x0237FD80`, `src/ipc/tr_vol.h`): HE console `[vol] 30% at 0x0237fd80: pads 0,
   encoder ok, switch ok, bench request word +4` (`encoder none` / `switch none` if that control failed to open); over SWD `+0x00` `vol` = `0x564F001E` (30 %), `+0x08` `rejects` 0, `+0x0C` `seq` 0. Turn the
   encoder: 5 % a detent, `[vol] 35% (seq 1)` (clockwise, from the 30 % boot level), the HUD shows `VOLUME 35%` for 1.5 s over its bottom row, `vol`
   follows, and the sound gets louder inside one 16 ms block with no click. HOLD the switch for 1 s (a long press; it fires while held): `MUTE`, the
   amps keep running (`hp_i2s_fu` stays 0, SD_N is never touched), hold again: the level it muted; turning while muted unmutes and steps from that saved level. A SHORT press (under 1 s) is not a mute any more: it switches the knob to BRIGHTNESS (HUD `BRIGHTNESS 30%`, console `[bl] backlight 35% -> 0` on the first detent), and a short press while muted only switches the mode (a long press unmutes). BRIGHTNESS is 10..80 % in 5 % steps (never dark, never past 80 %) and returns to VOLUME after 5 s with no turn or press. The first bench run found the phase order reversed (clockwise lowered it) and the
   overlay now lists ENC0_Y / P3_1 first; the rotary decode over GPIO3 (#2037 / #2095) and the switch (GPIO4 P4_3) are bench-exercised on that run only, not yet on the final order. If the encoder is dead, the request word below still works.

**Changing the backlight without reflashing (record `TR_MEM_BL` = `0x0237FDC0`, `src/ipc/tr_knob.h`).** Console
`[vol] ... backlight 30% at 0x0237fdc0`. Words: `+0x00` `bl` (`0x424C001E` at the 30 % boot level), `+0x04` `req`,
`+0x08` `rejects`, `+0x0C` `seq`. Write the REQUEST word, never `bl`: 10..80 % only (`0x424C000A` .. `0x424C0050`);
anything else (below 10 %, above 80 %, a wrong tag) is counted in `rejects` and ignored, so the panel can be neither turned dark nor past 80 %.

```
J-Link> w4 0x0237FDC4, 0x424C003C     // 60 %      (0x424C0000 | percent)
J-Link> mem32 0x0237FDC0, 4           // bl, req, rejects, seq
```

**Changing the volume without reflashing (any SWD probe, HE or HP running, nothing halted).** The sound's
volume is 0..100 % of the build's `TR_SND_VOLUME` ceiling (128, the level heard as safe on the 2026W36-0002
speakers; 100 % is that level, the word can only turn it down). Write the REQUEST word, never `vol`:
the HE is `vol`'s only writer and puts its own level back on the next frame.

```
J-Link> w4 0x0237FD84, 0x564F001E     // 30 %      (0x564F0000 | percent)
J-Link> mem32 0x0237FD80, 4           // vol, req, rejects, seq
```

| Level | `req` |
| --- | --- |
| mute (amps keep running) | `0x564F0000` |
| 25 % | `0x564F0019` |
| 50 % | `0x564F0032` |
| 75 % | `0x564F004B` |
| 100 % | `0x564F0064` |
| 30 % (boot default) | `0x564F001E` |

The HE adopts a request when the word CHANGES: writing the value it already holds again does nothing, and
writing `0` first (`0` is "no request", not a refusal) is the way to resend it after a local change. Anything without the `0x564F` tag, or with a
percent above 100, is refused (the word `0` itself is not): `rejects` (`+0x08`) counts it and the level stays. A `req` left over from
before an HE boot is ignored (the HE records it at boot), so write after the HE console says `[vol]`.
A cold SRAM0 reads as the default, 30 %, on both cores (the HP plays at 30 % from its first block, before the HE has published): a garbage `vol` word is never a level. Because the
change goes through the HE, it shows on the HUD and bumps `seq` (`+0x0C`).

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
build with `-DTR_CAM_ROTATE=0 -DTR_CAM_MIRROR=ON` (E1M-EVK; `90` for a camera on its side)):

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
