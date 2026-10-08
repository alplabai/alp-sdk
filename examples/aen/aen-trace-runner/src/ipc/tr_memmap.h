/* src/ipc/tr_memmap.h -- the fixed-address regions of the A32 build that
 * both sides name: a32/renderer/render.c + renderer.c place their buffers
 * with these, and the HE's HUD perf panel (src/hud/hud.c tr_mem_map())
 * counts them, so the SRAM figure cannot drift from where things really are.
 * Layout: docs/superpowers/plans/2026-09-22-a32-renderer.md section 4.
 * Framebuffers, mailbox and HUD buffer: tr_mbox.h; stub regions:
 * a32/common/stub_abi.h. Plain defines only; the renderer's start.S keeps
 * its stack tops as .equ literals (TR_MEM_A32_STACKS + 64 KiB per core). */
#ifndef TR_MEMMAP_H
#define TR_MEMMAP_H

/* SRAM0 */
#define TR_MEM_A32_SETUP 0x02200000u /* triangle setup records (render.c SETUP) */
#define TR_MEM_A32_BANDS \
	0x0229A000u /* per core a z + a colour band (render.c ZBAND), setup ends 0x022996EB */
#define TR_MEM_A32_ZTEX \
	0x022D8000u /* P15: the camera-side zone's ground textures, RGB565 (r3d_scene.c) */
#define TR_MEM_A32_ZTEX_SIZE 0x10000u /* 2 x 128 x 128 x 2 B */
#define TR_MEM_A32_ZIDX \
	0x022E8000u /* the six ground slots' texel indices, unpacked from 4 bpp (r3d_scene.c) */
#define TR_MEM_A32_ZIDX_SIZE \
	0x18000u /* 6 x 128 x 128 x 1 B: the four bound slots + the far road's two (r3d_scene.c) */
#define TR_MEM_A32_DL1 0x02300000u /* scene part 2's DL (render.c DL1) */
#define TR_MEM_NPU_ARENA \
	0x02339000u /* HP: MoveNet cut-model inference arena (docs/superpowers/specs/
                                       * 2026-09-24-npu-body-control-design.md sec 2/9, candidate (a)).
                                       * Above DL1's current end (TR_DL_MAX_TRIS trimmed to make room,
                                       * r3d.h), 4 KiB aligned; a32/renderer/render.c asserts DL1 does
                                       * not run into it. Ends EXACTLY at TR_MEM_ARING below -- zero
                                       * spare, ruled that way on real silicon (see TR_MEM_NPU_ARENA_SIZE)
                                       * after Vela's own 277.5 KiB figure proved short by 424 B once
                                       * TFLM's persistent allocations were counted too. */
#define TR_MEM_NPU_ARENA_SIZE \
	286720u /* 0x46000, 280 KiB. Vela's --optimise Size figure (277.5 KiB,
                                       * 284,160 B) undercounted TFLM's own persistent allocations on
                                       * top of it -- real silicon (HP ram console): "Failed to resize
                                       * buffer. Requested: 284288, available 283864, missing: 424."
                                       * (alp_inference_open ALP_ERR_IO). 286,720 B leaves ~2.4 KiB
                                       * headroom over the 284,288 B TFLM actually asked for -- ponytail:
                                       * untested against a second real allocation; if this ever fails
                                       * again, the ram console's "missing: N" line names the exact B to
                                       * add here. */
#define TR_MEM_ARING      0x0237F000u /* P10 HE -> HP sound ring, layout in tr_aring.h */
#define TR_MEM_ARING_SIZE 0x1C0u      /* == sizeof(tr_aring_t), asserted there */
#define TR_MEM_PSLOT \
	0x0237F200u /* HP -> HE pose slot, layout + protocol in tr_pslot.h (design:
                                       * docs/superpowers/specs/2026-09-24-npu-body-control-design.md
                                       * sec 4). probe/npu's OWN (unrelated, 'RUPN'-magic) result block
                                       * also lives here -- the two never run resident at once. */
#define TR_MEM_SRAM1_READY \
	0x0237FC90u /* HE -> HP: SRAM1 confirmed answering (src/platform/a32.c
                                        * sram1_answers(), the SAME check tr_a32_boot() already
                                        * gates its own mailbox use on) written here AFTER it,
                                        * so the HP can gate its camera pool (CAM_POOL, SRAM1
                                        * 0x02480000) on it WITHOUT ever touching SRAM1 itself to
                                        * find out -- design fix round 2. This address is SRAM0
                                        * (always-on on this board), right after the pslot
                                        * (0x0237F200 + 0xA84 = 0x0237FC84, 16-B aligned up),
                                        * still inside the sound ring's page.
                                        *
                                        * FIXED HERE ON PURPOSE, ahead of TR_MEM_HP_DBG below, not
                                        * after it: fix round 5 grew hp_dbg_t and moved THIS
                                        * constant out from under it (0x0237FCE0 -> 0x0237FCF0) to
                                        * make room -- silicon then paired that NEW HE build (new
                                        * address) against an OLD, already-flashed HP image (still
                                        * compiled with the old address), so the HE's ready write
                                        * landed somewhere the old HP never polled: the HP sat
                                        * forever in TR_HP_STATE_SRAM1_NOT_READY, and the HE's own
                                        * capture_box() timeout ("camera unresponsive after 450
                                        * ticks") was the only visible symptom, ~15 s later and one
                                        * layer removed from the real cause (fix round 6). A single
                                        * bare ready-word can't self-version its own address, so the
                                        * actual fix is structural: reserved 16 B, ahead of the
                                        * variable-size debug block, so a future hp_dbg_t growth
                                        * spends ITS OWN budget (0x0237FCA0 below, TR_MEM_ARING+0x360
                                        * of headroom -- see TR_MEM_HP_DBG) and never has to move
                                        * this address again. main.c's fall_back() also now prints
                                        * the pslot's last hp_state on a timeout, so this failure
                                        * mode reads as "HP stuck: SRAM1_NOT_READY" directly in the
                                        * HE console next time, not just "camera unresponsive". */
#define TR_MEM_SRAM1_READY_MAGIC 0x52315352u /* 'RSR1' (SRAM1 Ready) */
#define TR_MEM_I2C1_HANDOVER \
	0x0237FC94u /* HE -> HP: I2C1 handed over (the alp,i2c-handover nodes, i2c_handover_he.overlay
                                       * and hp_vision's board overlay, carry this as flag-address; TR_INPUT_NPU
                                       * builds). The next word of TR_MEM_SRAM1_READY's reserved 16 B
                                       * (0x0237FC90..0x0237FC9F): three words, 0x0237FC94 state, +4 nonce, +8 consumed
                                       * (zephyr/soc-bridge/alif/i2c_handover.h), still clear of TR_MEM_HP_DBG. */
#define TR_MEM_HP_DBG \
	0x0237FCA0u /* hp_vision's bench-readable per-stage DWT timing block
                                       * (src/ipc/tr_hp_dbg.h hp_dbg_t, shared with the HE's HUD
                                       * since fix round 5): right after TR_MEM_SRAM1_READY's
                                       * reserved 16 B above (0x0237FC90 + 0x10), 16-B aligned.
                                       * Ends at 0x0237FCA0 + sizeof(hp_dbg_t) -- fix round 12
                                       * (review + finding A's AE register readback fields): 0x68,
                                       * not the 0x58 an earlier round's comment claimed (round 5's
                                       * own field list alone totals 0x58; the struct's OVERALL
                                       * alignment is 8, uint64_t busy_cyc/total_cyc's own
                                       * requirement, so sizeof() pads to a multiple of 8 -- fix
                                       * round 11's seq took it to 0x60, this round's ae_reg_* took
                                       * it to 0x68; tr_hp_dbg.h's own _Static_assert pins the real
                                       * number, not a comment alone) = 0x0237FD08. TR_MEM_CAM_VIEW
                                       * below leaves 0x18 (24 B) of headroom past that before it
                                       * actually starts, not the zero-slack exact fit fix round 11
                                       * left (a struct that ends EXACTLY where the next one begins
                                       * has no room to grow without a relocation, the same class of
                                       * churn TR_MEM_SRAM1_READY's own comment already describes
                                       * fixing once). */
#define TR_MEM_CAM_VIEW \
	0x0237FD20u /* HP -> A32: live camera view descriptor (src/ipc/
                                       * tr_cam_view.h tr_cam_view_t, fix round 7 item 5), 32-B
                                       * aligned, TR_MEM_HP_DBG's own end (0x0237FD08) + headroom
                                       * up to this 32-B boundary (fix round 12). NOT the pixels
                                       * (those stay in TR_MEM_CAM_POOL, SRAM1, below) -- just
                                       * {buf_addr, frame_no, width, height} + a seqlock, so the
                                       * A32 renderer knows which CAM_POOL buffer to read this
                                       * frame. Ends at
                                       * 0x0237FD20 + sizeof(tr_cam_view_t) (28 B, v2's rotate) =
                                       * 0x0237FD3C, with 0x0237FD3C..0x0237FFFF (0x2C4 B) of headroom
                                       * still free in the sound ring's page after it. */
#define TR_MEM_BUS2 \
	0x0237FD40u /* HE <-> HP: the I2C2 + GPIO5 lease (src/ipc/tr_bus2.h tr_bus2_t, 48 B, TR_HP_SOUND
                                       * builds only). 64-B aligned, right after TR_MEM_CAM_VIEW's end
                                       * (0x0237FD3C), clear of TR_MEM_I2C1_HANDOVER (0x0237FC94..0x0237FC9F:
                                       * a different bus, a different protocol) and of hp_dbg; ends at
                                       * 0x0237FD70, still inside the shared NC page (0x0237F000..0x0237FFFF).
                                       * tr_bus2.h asserts all of that. */
/* SRAM1 */
#define TR_MEM_CAM_POOL \
	0x02480000u /* HP: OV9281 camera frame pool (design sec 2), 2 x 256,000 B GREY8;
                                          * hp_vision/boards/<board>.overlay's sram1_cam_pool DT node is the
                                          * binding source, this is documentation only for cross-referencing */
#define TR_MEM_CAM_POOL_SIZE 0x80000u /* 512 KiB, CONFIG_VIDEO_BUFFER_POOL_HEAP_SIZE */
#define TR_MEM_MBOX_PAGE_END \
	0x02403000u /* mailbox, prof + bench blocks, renderer L2 tables (0x023, 0x027, 0x021,
                                             * 0x024 -- fix round 10, camera pool cacheable) */
#define TR_MEM_RENDER_TTB 0x02408000u /* renderer L1 table, 16 KiB */
#define TR_MEM_A32_IMG_END \
	0x025C0000u /* renderer image + .bss end (renderer.ld); the image alone <= 512 KiB (STUB_PAYLOAD_LIMIT) */
/* The frame's DL (render.c DL), after the stub stacks (STUB_STACK1_TOP); Normal WB-WA S=1 XN in the
 * renderer table (the rest of 0x024xxxxx below it is NC exec, the stub contract). */
#define TR_MEM_A32_DL 0x02424000u
/* Band bins (render.c BINS). 0x02460000, not right after the DL (0x0245CFD8): 12 KiB of DL headroom. */
#define TR_MEM_A32_BINS 0x02460000u
/* Renderer stacks, 64 KiB per core (start.S), directly above the image cap. */
#define TR_MEM_A32_STACKS      0x025C0000u
#define TR_MEM_A32_STACKS_SIZE 0x20000u
/* Renderer core-1 gate word (renderer.c RENDER_GATE): the page above the stacks, outside .bss. */
#define TR_MEM_A32_GATE 0x025E0000u
/* TF-A RW, never mapped by the A32. Bench-verified on EVK-03 (2026W36-0002): 0x027DE000 is the
 * lowest TF-A-owned address (bl32 map RAM ORIGIN 0x027de000, literal pool 027de000 027ed000;
 * 0x027C2000..0x027DDFFF untouched through boot + 90 s), so FB B may end exactly here. */
#define TR_MEM_TFA_RW     0x027DE000u
#define TR_MEM_TFA_RW_END 0x027ED000u

#endif /* TR_MEMMAP_H */
