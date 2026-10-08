### Changed — the trace-runner is played with the arms: left arm up is one lane left, right arm up one lane right, both arms up a jump; the camera view is landscape on the EVK

The player no longer steps sideways to change lane or jumps with the whole body. Raising the player's
left arm moves one lane left, the right arm one lane right, both arms together jump; the crouch duck
is unchanged. "Raised" is the wrist more than `TR_ARM_UP_PCT` (50 %) of the shoulder width above the
same-side shoulder, with both keypoints at or above `TR_ARM_KP_MIN` (77); it is edge-triggered, and the
arm re-arms only below `TR_ARM_DOWN_PCT` (20 %). A lane step waits `TR_ARM_SETTLE_POSES` (4 poses, about
100 ms) for the other arm, so a both-arms raise is a jump and never also a lane step. An arm already up
when a player joins, a run restarts or a pause ends is spent until it is lowered. Which arm is the
player's left follows `TR_CAM_MIRROR` and the shoulders' x order rather than MoveNet's labels, so it
holds at every `TR_CAM_ROTATE`; the HE image gains the `TR_CAM_MIRROR` CMake option (default `ON`),
which must match the `hp_vision` build.

The rotation `TR_CAM_ROTATE=0` is the EVK bench release: the OV9281 is mounted upright and shown as a
640x400 landscape picture at native 1:1, letterboxed in the portrait camera half with the lamps above it
and the label below. `90` and `270` (a camera on its side) keep their portrait layout. The lamps read
LEFT ARM, RIGHT ARM, BOTH ARMS and DUCK. `build-release.sh` now also refuses an HE and HP build that
disagree on `TR_CAM_MIRROR` or on whether the rotation is `0`.

Removed with the old scheme: the lateral-position lane bands (`TR_TRACK_HYST_PX`, `TR_CAM_MIRROR_X`,
the lane fields of `tr_track_t`), the torso-lift jump (`TR_TRACK_JUMP_*`, `TR_TRACK_WIN`,
`TR_TRACK_LAND_COOLDOWN_FRAMES`), and the frame-width and game-lane arguments of `tr_track_init()`,
`tr_track_calibrate()`, `tr_track_resync()`, `tr_ctl_step()`, `tr_ctl_reset()` and `tr_attract_step()`. The classical
`TR_CAMERA` detector has no keypoints and so no lane or jump control; IMU tilt is untouched.
