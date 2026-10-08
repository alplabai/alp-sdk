### Changed — the trace-runner fills the Riverdi natively at 800 wide, 3/5 game over 2/5 camera, with a power graph on the HUD

The picture is rendered 800 columns wide (`TR_R3D_W`, was 720) and written to the panel `fw` columns wide: the
Riverdi RVT121 shows all 800 on its whole 1280x800 window (no bars), the RK055 its centre 720, cropped, never
scaled. `fw` is a new `uint16_t` in the mailbox frame (`tr_frame_in_t`, offset 174, the old trailing pad; still
mailbox version 3 and 176 B): the HE sets it from the CDC200 layer-1 size, and the renderer faults (pad3[7]
`0xAB1D0003 | fw/16 << 8`) on a frame whose `fw` is not a multiple of 16 in 16..800, so a Stage 0 HE (`fw` 0) and
the new renderer cannot run together. The screen is split 3/5 game (`TR_VIEW_H` 768, 24 bands) and 2/5 camera (512
rows, 16 bands) for every panel.

The landscape camera (`TR_CAM_ROTATE=0`) is scaled up x1.28 (32/25) to cover its area, about 10 px cropped a
side, by a NEON bilinear resample (a `vtbl4` gather per 32-column pattern, each source row filtered once),
bit-exact against its scalar reference under qemu; the skeleton is placed by the exact inverse map. The lamps and
the label sit on an opaque plate along the bottom edge, laid out over the panel's visible columns. A camera the HP
turned (90 / 270) is no longer drawn (`ROT nn` in the label); the portrait path and `tr_cam_rot_rows_neon` are gone.

The HUD stays 720 wide, centred on the Riverdi; its new rightmost tile shows the carrier +5V net's power ("+5V net
(SoM+LCD)"): about 10 s at 10 Hz as a 96x48 graph with now / avg / peak in mW, and a hole for every sample the HE
could not take while the HP holds I2C2. For that the INA236 `CONFIG` is now `0x485F` (AVG 128 of 204 us + 588 us,
101 ms a result, was `0x4927`: 282 ms) and `RAIL5V_PERIOD_MS` 100. `panel_rot.h` takes the picture width:
`tr_rot_idx`, `tr_rot_blit` and `tr_rot_blit_neon` gained a `pw` argument.
