# Unbounded Length Sweep — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Issue: #1645** (`bug`, `area:drivers`, `area:npu`, `needs-silicon`, milestone `Backlog`)

**Goal:** Close the six remaining sites where a length that came from the caller, the wire, or the far side of a link reaches a `memcpy`, a `read()`, or a DMA master without being checked against the destination.

**Architecture:** Four distinct failure shapes, not one — and the plan is organised by shape because the *fix* differs per shape, even though the *cause* is identical. Task 1 is the JPEG stride family (unvalidated caller geometry). Task 2 is the RPC/MQTT family (peer-controlled length silently truncated and reported as success). Task 3 is the DEEPX multi-input DMA, which is latent behind a default-off gate but must be fixed before that gate is ever flipped. Task 4 adds the cache-maintenance note the audit surfaced as a gap with no owner.

**Tech Stack:** C and C++ (clang-format 22.x, tabs), Zephyr, Yocto/Linux, ztest, twister on `native_sim/native/64`.

**Spec:** `docs/superpowers/plans/2026-08-23-post-audit-hardening-campaign.md` — read its **Global Constraints** and **Verification infrastructure** sections first.

## Global Constraints

- Base branch is `dev`. Verify with `git merge-base HEAD origin/dev`. Never `--base main`.
- Branch from an up-to-date `origin/dev`. **Campaign Step 0 (the 280 staged files) must be resolved first.**
- `bash scripts/test-all.sh --target dev` green before `gh pr create`.
- clang-format **22.x** on every changed `.c`/`.h`/`.cpp` including test files.
- After `git merge origin/dev`, run `python3 scripts/gen_catalog.py` and commit the result.
- No AI attribution anywhere.
- **Depends on Plan 1.** The two worst members of this family — `src/backends/i2s/zephyr_drv.c:218` (#1619) and `src/backends/can/zephyr_drv.c:228` (#1631) — are filed separately and are **not** in scope here. Reference them; do not restate or re-fix.

---

## Correcting a framing error before it costs a reviewer an hour

The audit grouped these eight sites under "caller- or peer-supplied length reaches a `memcpy` or a DMA master with no bound", which reads as eight memory-corruption bugs. **It is not.** Two of the eight are (both already filed). Of the six here:

| Shape | Sites | What actually goes wrong |
|---|---|---|
| Unvalidated caller geometry | 2 (JPEG) | Wrong output, or a raw pointer handed to an AXI master |
| Peer length silently truncated | 3 (RPC ×2, MQTT) | **Silent data loss reported as `ALP_OK`**, and one permanent connection wedge |
| Multi-input DMA over PCIe | 1 (DEEPX) | Reads past a heap allocation and DMAs it to the NPU — latent, gate default-off |

Only the DEEPX site is memory-unsafe today, and it is unreachable in a default build. **The RPC and MQTT sites are the ones a customer actually hits**, and they are worse than they look precisely because nothing crashes: `alp_rpc_call()` returns `ALP_OK` with a partial response, and the MQTT connection stops delivering forever after one oversized broker message.

Write the PR bodies to say this. A reviewer told "six memory-corruption bugs" who finds three silent-truncation bugs stops trusting the series.

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `src/jpeg_dispatch.c:89-91` | Modify: validate stride and enforce the advertised `max_width`/`max_height` | 1 |
| `src/backends/jpeg/vendor/toojpeg_baseline.c:588` | Modify: reject a stride smaller than the row it describes | 1 |
| `src/backends/jpeg/alif_hantro.c:264` | Modify: same check before the AXI master sees the pointer | 1 |
| `tests/zephyr/peripheral/src/jpeg_bounds.c` | Create: ztest for the stride rejections | 1 |
| `src/backends/rpc/yocto_uio_drv.c:757` | Modify: reject an over-long peer frame instead of clipping it | 2 |
| `src/backends/rpc/yocto_drv.c:366` | Modify: detect a truncated endpoint read | 2 |
| `src/backends/mqtt/zephyr_drv.c:260` | Modify: drain the payload remainder so the connection cannot wedge | 2 |
| `src/yocto/inference_deepx.cpp:279` | Modify: concatenate the inputs, or refuse a multi-input model | 3 |
| `src/common/alp_slot_claim.h` or a new note | Modify: record the cache-maintenance gap | 4 |

---

## Task 1: JPEG — the caller's geometry is never checked

**Files:** `src/jpeg_dispatch.c`, `src/backends/jpeg/vendor/toojpeg_baseline.c`, `src/backends/jpeg/alif_hantro.c`, `tests/zephyr/peripheral/src/jpeg_bounds.c` (create).

**The defect.** The dispatcher validates almost nothing:

```c
/* src/jpeg_dispatch.c:89-91 */
	if (req == NULL || out_buf == NULL || out_len == NULL || req->width == 0u ||
	    req->height == 0u) {
		rc = ALP_ERR_INVAL;
```

No stride is checked against its width, and the maximum the backend *advertises* is never enforced:

```c
/* src/backends/jpeg/sw_baseline.c:34-37 */
	*caps_out      = (alp_jpeg_caps_t){
		.hw_accelerated  = false,
		.mjpeg_supported = false,
		.max_width       = 16384u,
		.max_height      = 16384u,
```

Two consequences:

1. `src/backends/jpeg/vendor/toojpeg_baseline.c:588` walks rows by `y_stride` / `u_stride` / `v_stride`. A stride of `0` aliases **every row to row 0** and emits a structurally valid JPEG of the wrong image, with `rc = ALP_OK`. A stride smaller than `width` reads across row boundaries. Nothing rejects either.
2. `src/backends/jpeg/alif_hantro.c:264` derives the same geometry and hands a raw pointer to the JPEG **AXI master**. There, an under-sized stride is not merely a wrong picture — it is hardware reading memory the caller did not intend to expose.

- [ ] **Step 1: Create the branch**

```bash
git fetch origin
git checkout -b fix/1645-unbounded-length-sweep origin/dev
```

- [ ] **Step 2: Read `alp_jpeg_encode_req_t` before writing the check**

```bash
grep -n "y_stride\|u_stride\|v_stride\|} alp_jpeg_encode_req_t\|subsample" include/alp/jpeg.h
```

The correct minimum stride depends on the subsample mode — for `ALP_JPEG_SUBSAMPLE_420` the chroma planes are half-width, so `u_stride >= width / 2`, not `>= width`. **Get this from the header, not from intuition**: a check that rejects valid 4:2:0 input is worse than the bug, because it breaks working callers.

- [ ] **Step 3: Write the failing test**

Create `tests/zephyr/peripheral/src/jpeg_bounds.c`:

```c
#include <zephyr/ztest.h>
#include <string.h>

#include <alp/jpeg.h>
#include <alp/peripheral.h>

ZTEST(alp_peripheral, test_jpeg_rejects_zero_stride)
{
	alp_jpeg_t *h = alp_jpeg_open(NULL);
	if (h == NULL) {
		ztest_test_skip(); /* no jpeg backend in this image */
	}

	static uint8_t  plane[64 * 64];
	static uint8_t  out[8192];
	size_t          out_len = 0;

	alp_jpeg_encode_req_t req = {
		.width     = 64u,
		.height    = 64u,
		.subsample = ALP_JPEG_SUBSAMPLE_400,
		.y_plane   = plane,
		.y_stride  = 0u, /* the defect: aliases every row to row 0 */
	};

	zassert_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len), ALP_ERR_INVAL,
	              "a zero stride must be refused, not encoded as row 0 repeated");

	req.y_stride = 32u; /* smaller than width -- reads across row boundaries */
	zassert_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len), ALP_ERR_INVAL,
	              "a stride below width must be refused");

	req.y_stride = 64u; /* valid */
	zassert_not_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len), ALP_ERR_INVAL,
	                  "a correct stride must still be accepted");

	alp_jpeg_close(h);
}

ZTEST(alp_peripheral, test_jpeg_enforces_advertised_max)
{
	alp_jpeg_t *h = alp_jpeg_open(NULL);
	if (h == NULL) {
		ztest_test_skip();
	}
	alp_jpeg_caps_t caps;
	zassert_equal(alp_jpeg_capabilities(h, &caps), ALP_OK);

	static uint8_t out[256];
	size_t         out_len = 0;
	alp_jpeg_encode_req_t req = {
		.width  = caps.max_width + 1u,
		.height = 16u,
	};
	zassert_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len), ALP_ERR_OUT_OF_RANGE,
	              "width above the advertised max_width must be refused");

	alp_jpeg_close(h);
}
```

**Field names in `alp_jpeg_encode_req_t` and the `alp_jpeg_capabilities` signature must be taken from `include/alp/jpeg.h`** — the names above follow the audit's wording and were not individually verified. Fix them to match before running.

- [ ] **Step 4: Run it and confirm both tests FAIL**

```bash
west twister -p native_sim/native/64 -T tests/zephyr/peripheral --no-clean -v
```

Expected: zero-stride returns `ALP_OK` and produces output; the oversize width is accepted.

- [ ] **Step 5: Validate in the dispatcher, once**

Extend `src/jpeg_dispatch.c:89-91`. The dispatcher is the right home: the check is backend-independent, and putting it there means a future backend inherits it instead of re-deriving it.

Enforce, per plane that the subsample mode says is present: `stride != 0` and `stride >= plane_width_for(subsample)`. Then query the selected backend's `caps` and reject `req->width > caps.max_width` or `req->height > caps.max_height` with `ALP_ERR_OUT_OF_RANGE`.

**If the dispatcher does not already hold the caps**, read how another dispatcher reaches them (`alp_<class>_capabilities` is cached on the handle in several classes) rather than re-opening the backend.

- [ ] **Step 6: Defend the AXI master locally too**

`src/backends/jpeg/alif_hantro.c:264` must not rely solely on the dispatcher. A backend that hands a pointer to a DMA master validates its own inputs — the dispatcher check is the contract, the backend check is the thing standing between a bad caller and hardware reading arbitrary memory. Add the same stride assertion there, returning `ALP_ERR_INVAL`.

- [ ] **Step 7: Run the tests, format, gate, commit**

```bash
clang-format -i src/jpeg_dispatch.c src/backends/jpeg/vendor/toojpeg_baseline.c \
                src/backends/jpeg/alif_hantro.c tests/zephyr/peripheral/src/jpeg_bounds.c
git diff --exit-code
bash scripts/test-all.sh --target dev
git add -A
git commit -m "fix(jpeg): validate stride and enforce the advertised encode maximum

jpeg_dispatch checked only non-NULL and non-zero width/height, so a y_stride of
0 aliased every row to row 0 and emitted a structurally valid JPEG of the wrong
image with ALP_OK, and a stride below width read across row boundaries. The
max_width/max_height the backends advertise (16384 in sw_baseline) were never
enforced.

alif_hantro hands the same derived geometry to the JPEG AXI master, so an
under-sized stride there is hardware reading memory the caller did not intend
to expose -- it now validates locally as well as trusting the dispatcher."
```

---

## Task 2: Peer-controlled length, silently truncated

**Files:** `src/backends/rpc/yocto_uio_drv.c`, `src/backends/rpc/yocto_drv.c`, `src/backends/mqtt/zephyr_drv.c`.

**This is the task a customer feels.** Nothing here corrupts memory — every copy is bounded. What they do is lose data and report success.

### 2a — RPC over UIO clips the frame and dispatches it as complete

```c
/* src/backends/rpc/yocto_uio_drv.c:757-762 */
	unsigned char local_frame[ALP_RPC_TX_FRAME_MAX];
	size_t        frame_len = len < sizeof(local_frame) ? len : sizeof(local_frame);
	for (size_t i = 0; i < frame_len; ++i) {
		local_frame[i] = ((const volatile unsigned char *)data)[i];
	}
	data = local_frame;
	len  = frame_len;
```

The byte-wise copy above it is a real and well-documented fix (unaligned loads from Device/uncached UIO memory SIGBUS on ARMv8; bench cycle 11 crashed exactly there). **The clip is a separate, undocumented behaviour riding along with it.** A peer frame longer than `ALP_RPC_TX_FRAME_MAX` (1024, `src/backends/rpc/yocto_drv.c:142`) is silently cut to 1024 and then parsed and dispatched as if it were the whole message.

- [ ] **Step 1: Reject rather than clip**

```c
	unsigned char local_frame[ALP_RPC_TX_FRAME_MAX];
	if (len > sizeof(local_frame)) {
		/* A peer frame larger than the negotiated maximum is a protocol
		 * error, not something to silently truncate and dispatch as a
		 * complete message (#1645). */
		return; /* or the function's existing error path -- see below */
	}
```

**Read the enclosing function's signature and error convention first.** If it is a `void` rpmsg callback there may be no way to report upward, in which case the correct action is to drop the frame *and* record it somewhere the operator can see — check whether this backend has a diagnostic counter or a `LOG_ERR` already in use, and follow it. A silent drop is better than a silent truncation, but only barely.

### 2b — RPC over rpmsg char device truncates at `read()`

```c
/* src/backends/rpc/yocto_drv.c:346, :366 */
	uint8_t        buf[ALP_RPC_TX_FRAME_MAX];
	...
	ssize_t n = read(ch->ept_fd, buf, sizeof buf);
```

A peer message longer than the buffer is truncated by `read()` itself and the remainder stays queued in the endpoint, so the *next* read returns the tail of the previous message and every subsequent frame is misaligned by one.

- [ ] **Step 2: Detect the truncation**

The rpmsg endpoint delivers message-at-a-time, so a full-buffer read is the signal. After the read succeeds, if `(size_t)n == sizeof buf`, the frame was at least buffer-sized and may have been cut. Treat that as a protocol error on the same footing as 2a rather than parsing it.

- [ ] **Step 3: Make the two agree**

Both RPC backends must answer identically — this is exactly the backend-parity class Plan 3 (#1635) exists for. Whatever error path 2a takes, 2b takes.

### 2c — MQTT wedges permanently on one oversized broker message

```c
/* src/backends/mqtt/zephyr_drv.c:259-265 */
	/* Read payload directly into rx_buf -- bounded by buffer size. */
	size_t want = MIN(pub->message.payload.len, sizeof(be->rx_buf));
	size_t got  = 0;
	while (got < want) {
		int n = mqtt_read_publish_payload(client, be->rx_buf + got, want - got);
		if (n <= 0) break;
		got += (size_t)n;
	}
```

then, a few lines later, it acknowledges the message:

```c
/* src/backends/mqtt/zephyr_drv.c:268-271 */
		if (pub->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
			const struct mqtt_puback_param ack = { .message_id = pub->message_id };
			(void)mqtt_publish_qos1_ack(client, &ack);
		}
```

The `MIN` bounds the copy — **the memory-safety half of the audit's claim was correctly refuted.** The availability half stands and is worse: when `payload.len > sizeof(be->rx_buf)`, the loop reads only `want` and **never drains the remainder**. Zephyr's `remaining_payload` stays above zero, so `client_read` returns `-EBUSY` on every subsequent `mqtt_input`, and the connection stops delivering **forever** — after having already PUBACKed the message, so the broker considers it delivered and will not resend.

One broker-controlled length permanently bricks the MQTT link, with the message counted as received.

- [ ] **Step 4: Drain the remainder**

After the bounded read into `rx_buf`, consume and discard the rest so the client returns to a clean state:

```c
	/* Drain anything beyond rx_buf: leaving remaining_payload > 0 makes
	 * every later mqtt_input() return -EBUSY and the connection stops
	 * delivering permanently -- and we have already PUBACKed, so the
	 * broker will not resend (#1645). */
	size_t discarded = 0;
	while (discarded < (pub->message.payload.len - got)) {
		uint8_t sink[64];
		int     n = mqtt_read_publish_payload(client, sink,
		                                      MIN(sizeof(sink),
		                                          pub->message.payload.len - got - discarded));
		if (n <= 0) break;
		discarded += (size_t)n;
	}
```

- [ ] **Step 5: Tell the caller it was truncated**

The message callback currently receives `got` with no indication that bytes were dropped:

```c
	be->msg_cb((const char *)be->topic_buf, be->rx_buf, got, be->msg_user);
```

A subscriber handed a truncated payload with no signal will parse garbage. Check `include/alp/iot.h` for whether the callback signature can carry a truncation flag; if it cannot without an ABI change, at minimum `LOG_WRN` the drop with both lengths. **Do not silently deliver a truncated payload as if it were whole** — that is the same defect as 2a in a different subsystem.

- [ ] **Step 6: Format, gate, commit**

```bash
clang-format -i src/backends/rpc/yocto_uio_drv.c src/backends/rpc/yocto_drv.c \
                src/backends/mqtt/zephyr_drv.c
git diff --exit-code
bash scripts/test-all.sh --target dev
git commit -am "fix: stop reporting a truncated peer message as a complete one

rpc/yocto_uio_drv clipped a peer frame longer than ALP_RPC_TX_FRAME_MAX to 1024
and dispatched it as if whole; rpc/yocto_drv let read() truncate and left the
tail queued, misaligning every later frame. Both now treat an over-long frame as
a protocol error.

mqtt/zephyr_drv bounded its copy correctly but never drained the remainder, so
one broker message larger than rx_buf left remaining_payload > 0 and every later
mqtt_input() returned -EBUSY -- the connection stopped delivering permanently,
after the message had already been PUBACKed."
```

---

## Task 3: DEEPX multi-input DMA reads past the allocation

**Files:** `src/yocto/inference_deepx.cpp`.

**Latent, and must stay that way until fixed.** Gated behind `ALP_SDK_USE_DEEPX_DXM1` (`src/yocto/CMakeLists.txt:67`), default **OFF**, and marked BENCH-UNVERIFIED. It is not reachable in a shipping build today. It must be fixed **before** that gate is flipped for V2M bring-up, not after.

The struct's own comment states the contract:

```cpp
/* src/yocto/inference_deepx.cpp:93-97 */
	/* SDK-owned input staging buffers (one contiguous blob per input
     * tensor).  dx_rt's Run(inputPtr) takes a single pointer to the
     * concatenated inputs; for the common single-input model this is
     * just inputs[0]. */
	std::vector<std::vector<uint8_t>> input_bufs;
```

and the call violates it:

```cpp
/* src/yocto/inference_deepx.cpp:279 */
	void *input_ptr = st->input_bufs.empty() ? nullptr : st->input_bufs[0].data();
```

`input_bufs` is a `vector` of **separate** `vector<uint8_t>` allocations — they are not contiguous with each other. For any model with more than one input tensor, `dx_rt` reads `sum(size_in_bytes)` bytes starting at input 0's buffer, walks past that allocation into unrelated heap, and **DMAs it over PCIe to the DX-M1**.

- [ ] **Step 1: Choose — concatenate, or refuse**

Two defensible fixes, and the choice depends on how many multi-input models the V2M path must support:

- **Concatenate**: replace `std::vector<std::vector<uint8_t>>` with one flat `std::vector<uint8_t>` plus per-input offsets, so `data()` genuinely points at the concatenated blob the comment describes. Correct, and matches the documented dx_rt contract.
- **Refuse**: detect `input_bufs.size() > 1` at open and return `ALP_ERR_NOSUPPORT`. Honest, one-line, and correct until someone needs multi-input.

**Take the second unless a multi-input model is actually in scope for V2M bring-up.** A refusal that names the limitation is better than a concatenation written speculatively and never exercised on silicon — and this file is BENCH-UNVERIFIED, so the concatenation would ship untested.

```cpp
	if (st->input_bufs.size() > 1u) {
		/* dx_rt's Run() takes ONE pointer to the concatenated inputs, but
		 * input_bufs holds a separate allocation per tensor -- passing
		 * input_bufs[0].data() would have dx_rt read sum(size_in_bytes)
		 * past that allocation and DMA unrelated heap to the DX-M1 over
		 * PCIe (#1645).  Refuse until the staging buffer is flattened. */
		return ALP_ERR_NOSUPPORT;
	}
```

Put the check at **open**, not at `Run()` — a model that cannot run should fail when it is loaded, not on the first inference.

- [ ] **Step 2: Fix the unchecked narrowing cast while here**

The audit also found `src/yocto/inference_deepx.cpp:147-149` truncating a tensor dimension to 4 **and** casting `int64_t` to `uint16_t` unchecked. That is a separate defect in the same file; `src/yocto/inference_ort.cpp:651` pre-checks its dims against `UINT16_MAX` before the same truncation. Copy that check. (The three-way tensor-rank *contract* divergence is Plan 3's follow-up, not this — fix only the cast.)

- [ ] **Step 3: Format, gate, commit**

`ALP_SDK_USE_DEEPX_DXM1=OFF` by default means `test-all.sh` will not compile this file. Build it explicitly before committing, or the change is unverified:

```bash
clang-format -i src/yocto/inference_deepx.cpp
git diff --exit-code
# Compile the gated path -- see src/yocto/CMakeLists.txt:67 and :100 for how the
# option interacts with the recipe; a hand -DALP_SDK_USE_DEEPX_DXM1=ON is
# documented there as degrading cleanly.
bash scripts/test-all.sh --target dev
git commit -am "fix(deepx): refuse a multi-input model instead of DMAing past the allocation

DeepxState's own comment says dx_rt's Run() takes one pointer to the
concatenated inputs, but input_bufs is a vector of separate per-tensor
allocations. Passing input_bufs[0].data() had dx_rt read sum(size_in_bytes)
starting at input 0, walk into unrelated heap, and DMA it over PCIe to the
DX-M1.

Latent today (ALP_SDK_USE_DEEPX_DXM1 defaults OFF and the path is
BENCH-UNVERIFIED) -- fixed now so it cannot ship when the gate is flipped for
V2M bring-up. Also bounds the int64->uint16 dimension cast, matching
inference_ort.cpp:651."
```

- [ ] **Step 4: Bench — E1M-V2M101/102, before the gate is flipped**

This cannot be validated on `native_sim` and must not be validated by flipping the gate in CI. When V2M bring-up next has the DX-M1 on PCIe, load a single-input model and confirm inference still works, then load a multi-input model and confirm it is refused at open with `ALP_ERR_NOSUPPORT` rather than faulting.

---

## Task 4: The cache-maintenance gap

**Files:** a note; no functional change.

The audit found **zero cache maintenance anywhere in `src/`** — no clean, no invalidate — despite two AEN backends handing raw CPU pointers to DMA masters, and despite `include/alp/mproc.h` offering `alp_shmem_config_t.cacheable = true` with no flush/invalidate primitive behind it.

**Do not "fix" this here.** Adding cache maintenance to a DMA path without measuring on real silicon is how you turn a working system into an intermittently-corrupt one. What this task does is stop it being invisible:

- [ ] **Step 1: Record it where a DMA-path author will meet it**

Add a note to `include/alp/mproc.h` beside the `cacheable` field stating plainly that no flush/invalidate primitive exists yet, that `cacheable = false` is therefore the only safe setting on a coherent-DMA path today, and that the two AEN DMA-master backends do no maintenance either.

- [ ] **Step 2: Open the follow-up**

This wants its own issue with an owner and bench time — it is an architecture gap, not a sweep item. Reference #1645 in it and link back.

- [ ] **Step 3: Commit**

```bash
clang-format -i include/alp/mproc.h
git diff --exit-code
bash scripts/test-all.sh --target dev
git commit -am "docs(mproc): record that no cache-maintenance primitive exists

alp_shmem_config_t offers cacheable = true, but nothing in src/ performs a
clean or an invalidate anywhere, and the two AEN DMA-master backends hand raw
CPU pointers to hardware with no maintenance either. Documented rather than
fixed: adding barriers to a DMA path without measuring on real silicon trades a
visible gap for an intermittent one."
```

---

## Opening the PRs

Four PRs, all `--base dev`.

- Task 1: `Refs #1645.` Labels `bug`, `area:drivers`, `aen`.
- Task 2: `Refs #1645.` Labels `bug`, `area:drivers`, `v2n`.
- Task 3: `Refs #1645.` Labels `bug`, `area:npu`, `v2m`, `needs-silicon`.
- Task 4: `Closes #1645.` Labels `documentation`, `area:portability`.

**Bench:** Task 3 only, on E1M-V2M101/102 with the DX-M1 on PCIe, and it must happen before `ALP_SDK_USE_DEEPX_DXM1` is turned on for anything. Tasks 1, 2 and 4 are verifiable without silicon — though Task 2's MQTT wedge is worth reproducing against a real broker with an oversized retained message if one is easy to stand up, because the failure is a permanent hang rather than a crash and is easy to believe fixed when it is not.

**Deliberately not here:** #1619 (i2s) and #1631 (can) are the same family and already filed. The three-way tensor-rank contract across the inference backends is Plan 3's follow-up.
