/* probe/npu/src/payload.h -- the MRAM payload tools/npu_probe_payload.py builds
 * and the HP NPU probe reads: the Vela-compiled cut MoveNet, canned 640x400
 * GREY8 frames, and the host's expected results for each frame. Little-endian;
 * every offset is from the payload base and 16-aligned. */
#ifndef TR_NPU_PAYLOAD_H
#define TR_NPU_PAYLOAD_H

#include <stdint.h>

#define TR_NPU_PAYLOAD_ADDR \
	0x80100000u /* MRAM; clear of A32_APP (0x80020000) and the ATOC (0x80558000) */
#define TR_NPU_PAYLOAD_MAGIC   0x504E5254u /* 'TRNP' */
#define TR_NPU_PAYLOAD_VERSION 1u
#define TR_NPU_FRAMES_MAX      4u

typedef struct {
	int16_t x, y;
	uint8_t score, pad;
} tr_npu_kp_t;

typedef struct {
	uint32_t    map_crc[4]; /* CRC-32 (IEEE) of centre, heat, offset, regress: LiteRT reference */
	tr_npu_kp_t kp[17];     /* host decode of those maps (src/vision/movenet.c) */
	uint8_t     person;     /* 1 if tr_pose_box() is valid on the host */
	uint8_t     pad;
} tr_npu_expect_t;

typedef struct {
	uint32_t magic, version, total_len;
	uint32_t model_off, model_len, model_crc; /* the *_vela.tflite */
	uint32_t n_frames, frame_w, frame_h;
	uint32_t frame_off[TR_NPU_FRAMES_MAX];
	uint32_t expect_off; /* tr_npu_expect_t[n_frames] */
	char     accel[16];  /* Vela --accelerator-config, NUL-padded */
} tr_npu_payload_t;

_Static_assert(sizeof(tr_npu_kp_t) == 6, "tr_npu_kp_t is 6 B");
_Static_assert(sizeof(tr_npu_expect_t) == 120, "tr_npu_expect_t is 120 B");
_Static_assert(sizeof(tr_npu_payload_t) == 72, "tr_npu_payload_t is 72 B");

#endif /* TR_NPU_PAYLOAD_H */
