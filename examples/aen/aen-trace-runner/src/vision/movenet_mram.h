/* src/vision/movenet_mram.h -- where the release ATOC's build tooling
 * (a32/release/build-release.sh TR_HP_VISION=ON) puts the Vela'd cut
 * MoveNet model in MRAM, read in place by the NPU (no SRAM copy -- design
 * sec 2 pass B, docs/superpowers/specs/2026-09-24-npu-body-control-design.md).
 * Same address probe/npu's own payload already uses (tools/npu_probe_payload.py
 * TR_NPU_PAYLOAD_ADDR, probe/npu/src/payload.h) -- the probe and the real
 * hp_vision app are never flashed resident at once, so sharing the address
 * costs nothing and keeps one fewer MRAM region to track. Raw Vela'd
 * .tflite bytes only here, no probe framing (magic/CRC/canned frames). */
#ifndef TR_MOVENET_MRAM_H
#define TR_MOVENET_MRAM_H

#define TR_MOVENET_MRAM_ADDR 0x80100000u
#define TR_MOVENET_MRAM_SIZE \
	2429520u /* design doc's model table: cut model, Vela'd, --optimise Size */

#endif /* TR_MOVENET_MRAM_H */
