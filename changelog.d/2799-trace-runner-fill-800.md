### Changed — the trace-runner fills the Riverdi natively at 800 wide, 3/5 game over 2/5 camera, with a power graph on the HUD

Stage 0, the memory re-plan that makes room for it: a framebuffer slot is 800x1280x2 B = 2,048,000 B (`TR_FB_SLOT_SIZE`); FB B is
derived as the last slot below TF-A RW, `0x025EA000` (bench-verified base `0x027DE000`); the display list and the bins move to
SRAM1 (`0x02424000`, `0x02460000`), the band buffers to `0x0229A000`, the stacks to `0x025C0000` and the gate word to
`0x025E0000`. The HUD memory map and `TR_MEM_REGIONS` follow.

The picture is rendered 800 columns wide (`TR_R3D_W`, was 720) and written to the panel `fw` columns wide: the
Riverdi RVT121 shows all 800 on its whole 1280x800 window (no bars), the RK055 its centre 720, cropped, never
scaled. `fw` is a new `uint16_t` in the mailbox frame (`tr_frame_in_t`, offset 174, the old trailing pad, 176 B; the
mailbox version is 4 so a Stage 0 / 720-wide renderer and a Stage 1 HE can never pair silently): the HE sets it from the CDC200 layer-1 size, and the renderer faults (pad3[7]
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

Frame time, after the first bench run (peak 30.9 ms against the 31.2 ms real budget): the two cores claim the
bands fullest-bin first, the video bands last (`render_claim_order()`); and the scene's second half is no longer
copied onto the end of the first (`tr_dl_t` can read as two pieces, `tr_dl_tri()`), so setup and binning start
sooner. Neither changes a pixel (the goldens are unchanged). The power poll keeps a true 10 Hz: its deadline advances
a period from the last deadline instead of from the call, which the frame-rate quantised to 8.3 Hz.

The camera's focal length follows the panel (`tr_scene_f_px(fw)`, at most `(fw / 2) / 0.74`): 540 at 800 as before, 486.5 on
the RK055's 720 crop, where the taller viewport's larger focal length would have cut ~40 px off outer-lane obstacles.
The A32 sprite score is drawn at the panel's left edge on the crop. A rot-90/270 camera is not drawn (`ROT nn`).
