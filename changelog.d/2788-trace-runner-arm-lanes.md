### Changed — the trace-runner is played with the arms: left arm up is one lane left, right arm up one lane right, both arms up a jump; the camera view is landscape on the EVK

The player no longer steps sideways to change lane or jumps with the whole body. Raising the player's
left arm moves one lane left, the right arm one lane right, both arms together jump; the crouch duck
is unchanged. "Raised" is the wrist more than `TR_ARM_UP_PCT` (50 %) of the shoulder width above the
same-side shoulder, with both keypoints at or above `TR_ARM_KP_MIN` (77); it is edge-triggered, and the
arm re-arms only below `TR_ARM_DOWN_PCT` (20 %). A lane step waits `TR_ARM_SETTLE_POSES` (4 poses, about
100 ms, counted on every pose whether or not the wrist is still judged) for the other arm, so a
both-arms raise is a jump and never also a lane step; a wrist that left the frame or fell back into the
hysteresis band in that time never confirms its rise. The second arm of a staggered raise, while the
first is still up, is a jump too. An arm already up when a player joins (judged per arm), a run
restarts, the initials screen opens or a pause ends is spent until it is lowered. A jump asked during a
duck or the end of a jump is held `TR_JUMP_BUFFER_TICKS` (4) steps and taken when the runner is free. Which arm is the
player's left follows the shoulders' x order and whether the view is mirrored, not MoveNet's labels, so
it holds at every `TR_CAM_ROTATE`; a 180 degree mount (`TR_CAM_FLIP_Y`) swaps the sides once more. The
HE takes "mirrored" from the HP's camera descriptor (`tr_cam_view_t.mirror`, the sensor flip bit read
back) and falls back to its new `TR_CAM_MIRROR` CMake option (default `ON`, to match the `hp_vision`
build) until the HP publishes, printing a warning once if they differ. The new HP option
`TR_OV9281_HMIRROR_ACTIVE_LOW` reverses the rot-0 flip bit if the bench shows it mirrors the other way
(`FLASH-RECIPE.md` has the acceptance step).

The rotation `TR_CAM_ROTATE=0` is the EVK bench release: the OV9281 is mounted upright and shown as a
640x400 landscape picture at native 1:1, letterboxed in the portrait camera half with the lamps above it
and the label below. `90` and `270` (a camera on its side) keep their portrait layout. The lamps read
LEFT ARM, RIGHT ARM, BOTH ARMS and DUCK. `build-release.sh` now also refuses an HE and HP build that
disagree on `TR_CAM_MIRROR` or on whether the rotation is `0`, or an HE that is not `TR_INPUT_NPU=ON`,
and builds the A32 renderer with the HE's `TR_CAM_ROTATE` (`make -C a32/renderer TR_CAM_ROTATE=`) so the
boot layout matches before the HP publishes.

Removed with the old scheme: the lateral-position lane bands (`TR_TRACK_HYST_PX`, `TR_CAM_MIRROR_X`,
the lane fields of `tr_track_t`), the torso-lift jump (`TR_TRACK_JUMP_*`, `TR_TRACK_WIN`,
`TR_TRACK_LAND_COOLDOWN_FRAMES`), and the frame-width and game-lane arguments of `tr_track_init()`,
`tr_track_calibrate()`, `tr_track_resync()`, `tr_ctl_step()`, `tr_ctl_reset()` and `tr_attract_step()`. The classical
HE-side camera and detector (`TR_CAMERA`, `camera.conf`) had no keypoints and so no lane or jump: the
option is refused at configure time, use `TR_INPUT_NPU=ON`. IMU tilt is untouched.
