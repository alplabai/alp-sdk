/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * [vendor-ext] Renesas DRP-AI (DRP-AI TVM / MERA) backend hook for
 * <alp/inference.h>, A55 / Linux / Yocto side of the RZ/V2N.
 *
 * BENCH-UNVERIFIED: header-checks against the real
 * MeraDrpRuntimeWrapper.h surface and cross-compiles to a valid .o with
 * every previously-missing symbol defined -- confirmed with `nm` by hand
 * on an x86_64 dev host, NOT by a bake (see
 * meta-alp-sdk/recipes-renesas/mera2-drpai-tvm/mera2-drpai-tvm_2.7.0.bb).
 * The FINAL LINK against the real aarch64 obj/build_runtime/v2h
 * libraries has never been exercised: that same x86_64 host stops with
 * "skipping incompatible ... when searching for -lmera2_runtime", an
 * architecture mismatch, not proof of symbol resolution.  NO
 * `drpai`-enabled alp-image-edge bake has ever completed, on any host.
 * No compiled drpai_dir bundle exists in this checkout -- only an ONNX
 * source does (RUHMI's yolox-S_VOC.onnx); see docs/bring-up-drpai-v2n.md
 * Sec 5 for the current compile status and alp-sdk#2236 for the tracked
 * work (derive real preprocessing from app_yolox_cam, compile a bundle,
 * verify it byte-for-byte against its own input_0.bin).  Compiled only
 * when ALP_SDK_USE_DRPAI_V2N=ON (default OFF).  Same posture as the
 * DEEPX DX-M1 hook (inference_deepx.cpp).
 *
 * ----------------------------------------------------------------------
 * Real vendor API
 *   Written against the *real* DRP-AI TVM application runtime wrapper
 *   `MeraDrpRuntimeWrapper` (rzv_drp-ai_tvm/apps/MeraDrpRuntimeWrapper.h,
 *   EdgeCortix/Renesas).  Surface used here (all present in that header):
 *     - MeraDrpRuntimeWrapper()                          default ctor
 *     - bool LoadModel(const std::string& model_dir,
 *                      uint64_t start_address)           loads the .dat dir
 *     - void SetInput(int idx, const float*  data)
 *     - void SetInput(int idx, const uint16_t* data)     (fp16)
 *     - std::vector<std::tuple<std::string,size_t,InOutDataType>>
 *           GetInputInfo() / GetOutputInfo()
 *     - std::tuple<InOutDataType,void*,int64_t> GetOutput(int idx)
 *                                                        (dtype, ptr, elems)
 *     - void Run()
 *   `InOutDataType` is `enum class { FLOAT32, FLOAT16, INT32, INT64,
 *   OTHER }`.  The header also exposes GetInputDataType(int)/GetNumInput()/
 *   GetNumOutput(), but this body takes the input/output dtype straight
 *   from the InOutDataType in the GetInputInfo()/GetOutputInfo() tuple
 *   (std::get<2>) and the counts from those vectors' sizes, so those
 *   scalar accessors are part of the available surface but not invoked.
 *
 *   Header self-containedness: MeraDrpRuntimeWrapper.h uses std::tuple and
 *   std::vector but only #includes <string>/<memory>/<ostream> and
 *   <tvm/runtime/profiling.h> -- it does NOT pull in <tuple>/<vector>
 *   itself.  This file therefore #includes <tuple> and <vector> BEFORE the
 *   wrapper include below; that ordering is load-bearing, not incidental.
 *
 * Blob format ("drpai_dir")
 *   DRP-AI's compiled model is a multi-file object DIRECTORY (drp_desc.bin
 *   / weight.bin / addr_map.txt / deploy.json / deploy.so / preprocess/ ...)
 *   emitted by the host DRP-AI TVM compiler
 *   (scripts/alp_model/adapters/drpai.py).  It is NOT a single flat
 *   buffer, so the portable `.alpmodel` blob is the deterministic .tar of
 *   that object dir produced by adapters/drpai.py (`blob_format "drpai_dir"`).
 *   cfg.model_data / cfg.model_size carry those raw tar BYTES by value --
 *   exactly like the DEEPX path passes the raw .dxnn buffer, so the generic
 *   loader (src/common/alp_model_loader.c) stays format-agnostic.
 *
 *   This body owns the staging: open() extracts the tar into a private
 *   mkdtemp() directory (the .dat object files land flat -- drpai.py tars
 *   them relative to the object dir), calls LoadModel() on that dir, and
 *   close() removes the directory.  Extraction shells out to `tar -xf - -C
 *   <dir>` (busybox/GNU, A55/Yocto-side); the dir path is mkdtemp-private so
 *   there is no untrusted input in the command, and mkdtemp creates it 0700
 *   so another local user cannot race the extraction.  The blob size is
 *   capped at open() (kDrpAiMaxModelBytes) so it cannot fill /tmp.
 *
 * Vendor-artifact handling (classifying-public-vs-internal)
 *   rzv_drp-ai_tvm is Apache-2.0 (referenceable) BUT the prebuilt MERA2
 *   runtime libs + the DRP-AI Translator are Renesas/EdgeCortix
 *   account-gated.  Neither the wrapper sources nor the binaries are
 *   vendored here; this body links against them via the RZ/V Yocto SDK
 *   sysroot at build time.  Account-gated binaries belong in
 *   alp-sdk-internal (Git LFS).
 *
 *   Follow-up: place MeraDrpRuntimeWrapper.{h,cpp} + the MERA2 prebuilt
 *   libs in alp-sdk-internal and wire the RZ/V Yocto SDK sysroot find
 *   into src/yocto/CMakeLists.txt's ALP_SDK_USE_DRPAI_V2N block so the
 *   cross-build resolves mera2_runtime/drp_tvm_rt/tvm_runtime.
 *
 * DRP-AI working-memory arena
 *   LoadModel()'s second argument is the PHYSICAL base of the DRP-AI
 *   reserved working-memory region -- the arena the runtime allocates the
 *   model's DRP descriptors, weights and I/O buffers out of, and which the
 *   DRP-AI hardware DMAs against directly.  It is board DT policy, never a
 *   compile-time constant: a wrong base does not fail loudly, it makes the
 *   NPU scribble over whatever else owns that RAM.  _drpai_mem_start()
 *   below asks the driver, exactly as all 16 vendor call sites do.
 *
 *   The DT is the single source of truth for that arena -- its carve-outs,
 *   which of them &drpai0 claims, and why both memory properties are
 *   mandatory are documented once, at the &drpai0 node in
 *   meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-drpai.dtsi.
 *   (e1m-v2n-som.dtsi declares the reserved-memory carve-outs and
 *   #includes that file; the override itself lives apart because the
 *   `drpai0` label only exists when the optional meta-rz-drpai layer is in
 *   bblayers.conf.)  None of it is restated here; this file only ASKS the
 *   driver.
 *
 * Several models, one process
 *   The NPU runs ONE job at a time and the driver has no queue (a second
 *   DRPAI_START gets -EBUSY).  So (a) each open handle is loaded into its
 *   own range of the arena -- drpai_arena.h places it after the previous
 *   one using the runtime's GetLastAddress() (real wrapper, rzv_drp-ai_tvm
 *   Release-2026-04-17 apps/MeraDrpRuntimeWrapper.h: `uint64_t
 *   GetLastAddress()`, an absolute end address, 0 for a CPU-only model),
 *   leaving the arena tail free
 *   for pre-processing, and open() returns ALP_ERR_NOMEM when it no longer
 *   fits -- and (b) invoke() holds one process-wide mutex across SetInput +
 *   Run, so threads on different handles take turns.  Two PROCESSES are not
 *   coordinated: both would load at the arena base.
 *
 * Dispatcher contract
 *   Mirrors the 7-symbol hook shape the Yocto dispatcher in
 *   inference_yocto.c calls.  The handle layout (struct alp_inference)
 *   is the shared definition in inference_handle_internal.h (issue #1257).
 */

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <tuple>
#include <vector>

/* Target-only.  <linux/drpai.h> is the DRP-AI driver uapi header; it ships
 * into the RZ/V sysroot as ${includedir}/linux/drpai.h from meta-rz-drpai's
 * drpai_1.4.0 recipe, so the ALP_SDK_USE_DRPAI_V2N=ON build must carry a
 * `drpai` DEPENDS (recipe-side; not this file's to add). */
/* Before <linux/drpai.h>: the test fake of that header redefines open()/ioctl()
 * for the code that follows it, and drpai_arena.h opens its own lock file. */
#include "drpai_arena.h"

#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <linux/drpai.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "MeraDrpRuntimeWrapper.h"

#include "drpai_deploy_shapes.h"

extern "C" {
#include "alp/inference.h"

#include "inference_handle_internal.h"
}

/* The dispatcher's `struct alp_inference` comes from the shared internal
 * header (issue #1257) -- this file used to hand-mirror the layout, with a
 * different field order that only worked because pointers are 8 bytes. */

namespace
{

/* Pulls in JsonValue/JsonParser/DeployShapes/_drpai_parse_deploy_shapes
 * (drpai_deploy_shapes.h) unqualified -- see the "deploy.json rank/shape
 * recovery" comment below. */
using namespace alp_drpai;

/* DRP-AI driver device node.  Uniform across every vendor sample. */
constexpr const char *kDrpAiDevice = "/dev/drpai0";
/** Upper bound on a `drpai_dir` tar blob accepted by open(). */
constexpr std::size_t kDrpAiMaxModelBytes = 64u * 1024u * 1024u;

/** Map a DRP-AI driver errno onto the portable status enum.
 *
 *  Two of the driver's failure modes are transient and worth retrying, so
 *  they do not collapse into the ALP_ERR_IO catch-all:
 *    - ETIMEDOUT                   down_timeout(&priv->sem, MAX_SEM_TIMEOUT)
 *                                  expired -- 1000 ms; someone else holds the
 *                                  driver.
 *    - EINPROGRESS / EADDRNOTAVAIL the V2N shared-memory exclusion lock
 *                                  (R_DRPAI_LockDrpaiContStatus) is contended
 *                                  or already held.
 *  Everything else stays ALP_ERR_IO -- notably ENOENT, i.e. /dev/drpai0 is
 *  absent because &drpai0 never got enabled, which is what lets a caller
 *  tell "no DRP-AI on this board" from "busy, retry".
 */
alp_status_t _drpai_errno_to_status(int err)
{
	switch (err) {
	case ETIMEDOUT:
		return ALP_ERR_TIMEOUT;
	case EINPROGRESS:
	case EADDRNOTAVAIL:
		return ALP_ERR_BUSY;
	default:
		return ALP_ERR_IO;
	}
}

/** Resolve the physical base of the DRP-AI reserved working-memory arena
 *  and return it in @p out -- the value LoadModel() takes as its start
 *  address (see the "DRP-AI working-memory arena" note in the file header).
 *
 *  Asked of the driver, never hard-coded: the driver returns exactly the
 *  base its DT `memory-region` phandle resolves to, so this tracks the
 *  board DT instead of duplicating it.  A stale constant here would point
 *  the NPU's DMA at whatever else owns that RAM -- on the Alp SoM the old
 *  0x80000000 was `mmp_reserved: linux,multimedia`, the mmngr video buffer
 *  pool, not the NPU carve-out at 0xd0000000.
 *
 *  A fresh fd per call is deliberate, and it is NOT free.
 *
 *  Deliberate: DRPAI_GET_DRPAI_AREA is stateful per-fd.  The alternating
 *  cursor is `get_drpai_area_count` in the per-fd drpai_rw_status, zeroed in
 *  drpai_open() and toggled only when drpai_region2_size != 0, so a fresh fd
 *  always yields region 1 -- the arena the runtime wants, and what the vendor
 *  samples do.
 *
 *  Not free: drpai_open() is not a cheap open().  It takes
 *  down_timeout(&priv->sem, MAX_SEM_TIMEOUT) (1000 ms), takes the
 *  shared-memory exclusion lock, and when refcount == 1 runs
 *  drpai_open_process(); the matching ::close(fd) runs drpai_close_process(),
 *  which RESETS the DRP-AI, when it is the sole opener.  So on a first
 *  alp_inference_open() this probe power-cycles the NPU, and LoadModel() then
 *  opens its own fd and initialises it again.  Cheap enough at open() time,
 *  but do not call this per inference.
 *
 *  @p out_size receives the arena size (the runtime's GetLastAddress() is
 *  checked against it so several handles can share the arena -- see
 *  drpai_arena.h).
 *
 *  @return ALP_OK on success; otherwise the mapped driver errno --
 *          ALP_ERR_TIMEOUT / ALP_ERR_BUSY when the driver is contended,
 *          ALP_ERR_IO when the node is absent (i.e. &drpai0 was never
 *          enabled) or the ioctl fails for any other reason.
 */
alp_status_t _drpai_mem_start(uint64_t &out, uint64_t &out_size)
{
	const int fd = ::open(kDrpAiDevice, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		return _drpai_errno_to_status(errno);
	}

	drpai_data_t area = {};
	const int    rc   = ::ioctl(fd, DRPAI_GET_DRPAI_AREA, &area);
	/* Latch errno before ::close(), which clobbers it (and resets the
	 * DRP-AI -- see above). */
	const int err = errno;
	::close(fd);

	if (rc != 0) {
		return _drpai_errno_to_status(err);
	}
	/* A zero-sized or zero-based area means the driver probed without a
	 * usable memory-region; treat either as hard failure rather than point
	 * the NPU's DMA at physical 0 (the same hazard as a stale constant). */
	if (area.size == 0 || area.address == 0) {
		return ALP_ERR_IO;
	}

	out      = area.address;
	out_size = area.size;
	return ALP_OK;
}

/** Recursively remove a staging directory created by _stage_drpai_blob().
 *  No-op on an empty path.  Best-effort: the path is always an mkdtemp()
 *  result, so there is no untrusted input in the command. */
void _rm_rf(const std::string &dir)
{
	if (dir.empty()) {
		return;
	}
	std::string cmd = "rm -rf '" + dir + "'";
	/* Best-effort; bound, not cast: glibc marks system() warn_unused_result,
	 * which a (void) cast does not silence under GCC. */
	const int rc = std::system(cmd.c_str());
	(void)rc;
}

/** Extract the `drpai_dir` tar @p data (@p len bytes) into a fresh private
 *  directory and return its path in @p out_dir.
 *
 *  @return ALP_OK on success; ALP_ERR_NOMEM / ALP_ERR_IO on failure (the
 *          partially-created dir is cleaned up on failure).
 */
alp_status_t _stage_drpai_blob(const void *data, size_t len, std::string &out_dir)
{
	char        tmpl[] = "/tmp/alp-drpai-XXXXXX";
	const char *dir    = ::mkdtemp(tmpl);
	if (dir == nullptr) {
		return ALP_ERR_IO;
	}

	/* Pipe the tar bytes to `tar`'s stdin (-f -).  popen uses /bin/sh, but
     * the only interpolated token is our mkdtemp() path, so the command is
     * not attacker-influenced. */
	std::string cmd = "tar -xf - -C '" + std::string(dir) + "'";
	FILE       *p   = ::popen(cmd.c_str(), "w");
	if (p == nullptr) {
		_rm_rf(dir);
		return ALP_ERR_IO;
	}

	/* `tar` exits at the first bad header, so a write into its closed stdin
	 * raises SIGPIPE, whose default action killed the calling app
	 * (bench, E1M-V2M103: rc 141).  Block it on this thread for the write
	 * and pclose, then drain a pending one before restoring the mask; the
	 * short write / non-zero exit below turns it into ALP_ERR_IO. */
	sigset_t pipe_set, old_set;
	sigemptyset(&pipe_set);
	sigaddset(&pipe_set, SIGPIPE);
	const bool masked = ::pthread_sigmask(SIG_BLOCK, &pipe_set, &old_set) == 0;

	size_t wrote = (len > 0) ? std::fwrite(data, 1, len, p) : 0;
	int    rc    = ::pclose(p);

	if (masked) {
		const struct timespec zero = { 0, 0 };
		while (::sigtimedwait(&pipe_set, nullptr, &zero) == SIGPIPE) {
		}
		::pthread_sigmask(SIG_SETMASK, &old_set, nullptr);
	}
	if (wrote != len || rc != 0) {
		_rm_rf(dir);
		return ALP_ERR_IO;
	}

	out_dir = dir;
	return ALP_OK;
}

/** Per-handle DRP-AI state.  Owns the MERA runtime wrapper + SDK-owned
 *  input staging buffers + a snapshot of the I/O tensor metadata taken
 *  at open() time, plus the private staging dir extracted from the blob. */
struct DrpaiState {
	MeraDrpRuntimeWrapper runtime; /* default-constructed */
	std::string           model_dir;
	/* This handle's range in the process-wide arena (drpai_arena.h);
	 * close() gives it back. */
	alp_drpai::Range arena_range;
	/* mkdtemp() staging dir holding the extracted .dat object files; removed
     * (after the runtime is torn down) in close(). Empty if not staged. */
	std::string staged_dir;

	/* One SDK-owned input staging buffer per input tensor; the app fills
     * these via get_input(), invoke() pushes them with SetInput(). */
	std::vector<std::vector<uint8_t>> input_bufs;

	/* I/O metadata snapshots: (name, size_bytes, dtype). */
	std::vector<std::tuple<std::string, size_t, InOutDataType>> in_info;
	std::vector<std::tuple<std::string, size_t, InOutDataType>> out_info;

	/* Per-tensor shape, parallel to in_info/out_info; an empty entry means
     * rank stays 0 for that tensor (deploy.json missing/unreadable, this
     * tensor's rank exceeds 4 -- issue #1729, or its row didn't resolve).
     * Populated once at open() from deploy.json (see
     * _drpai_parse_deploy_shapes()); get_input()/get_output() just index
     * into it. */
	std::vector<std::vector<uint16_t>> in_shapes;
	std::vector<std::vector<uint16_t>> out_shapes;
};

/* ------------------------------------------------------------------ */
/* deploy.json rank/shape recovery (issue #1635).                      */
/*                                                                      */
/* GetInputInfo()/GetOutputInfo() give (name, size_bytes, dtype) only -- */
/* no shape.  deploy.json, the TVM graph-runtime JSON that              */
/* scripts/alp_model/adapters/drpai.py tars into every drpai_dir blob   */
/* alongside drp_desc.bin/weight.bin, carries it, and it's already on   */
/* disk in st->model_dir by the time open() reaches this.  alp-sdk      */
/* carries no JSON library (see the file header), so this is a minimal  */
/* scanner for exactly this one machine-generated structure -- not a    */
/* general parser -- kept file-local.  The JsonValue/JsonParser/         */
/* DeployShapes/_drpai_parse_deploy_shapes machinery itself lives in    */
/* drpai_deploy_shapes.h (included above, `alp_drpai` namespace, pulled */
/* in unqualified via the `using namespace alp_drpai;` at the top of    */
/* this anonymous namespace) so it can be unit-tested without the       */
/* RUHMI/DRP-AI TVM sysroot -- see                                      */
/* tests/native/drpai_deploy_shapes/test_deploy_shapes.cpp.             */
/* ------------------------------------------------------------------ */

/** Map a MERA InOutDataType onto the alp_inference dtype enum.  The DRP-AI
 *  wrapper reports FLOAT32 / FLOAT16 / INT32 / INT64 / OTHER; INT64 and
 *  OTHER have no portable slot, so they fall back to INT32 / UINT8
 *  respectively (the raw bytes stay reachable via the descriptor). */
alp_inference_dtype_t mera_dtype_to_alp(InOutDataType t)
{
	switch (t) {
	case InOutDataType::FLOAT32:
		return ALP_INFERENCE_DTYPE_F32;
	case InOutDataType::FLOAT16:
		return ALP_INFERENCE_DTYPE_F16;
	case InOutDataType::INT32:
		return ALP_INFERENCE_DTYPE_INT32;
	case InOutDataType::INT64:
		/* No 64-bit slot in the portable enum; report as int32 (callers
         * needing true int64 read raw bytes via size_bytes). */
		return ALP_INFERENCE_DTYPE_INT32;
	case InOutDataType::OTHER:
	default:
		return ALP_INFERENCE_DTYPE_UINT8;
	}
}

} /* namespace */

/* ------------------------------------------------------------------ */
/* Backend hooks (C ABI, matching inference_yocto.c's declarations).   */
/* ------------------------------------------------------------------ */

extern "C" alp_status_t alp_inference_drpai_open(struct alp_inference         *h_,
                                                 const alp_inference_config_t *cfg)
{
	struct alp_inference *h = h_;

	/* For ALP_INFERENCE_MODEL_DRPAI the blob is the `drpai_dir` tar bytes
     * (see the header comment).  Reject an empty blob early. */
	if (cfg->model_data == nullptr || cfg->model_size == 0) {
		return ALP_ERR_INVAL;
	}
	/* DRP-AI3 is one unit: accel_unit_mask may only be 0 (default) or bit 0. */
	if ((cfg->accel_unit_mask & ~1u) != 0u) {
		return ALP_ERR_NOSUPPORT;
	}
	/* The tar is extracted into /tmp before LoadModel() reads it; bound it
	 * so an oversized blob is a portable INVAL, not a tar ENOSPC on a
	 * small rootfs.  64 MiB is generous for a DRP-AI YOLOX-S bundle. */
	if (cfg->model_size > kDrpAiMaxModelBytes) {
		return ALP_ERR_INVAL;
	}

	/* Ask the driver for the working-memory arena base FIRST, before any
     * allocation or staging: a board with no DRP-AI (or a busy one) then
     * fails here, without the untar-then-rm_rf round trip.  No constant
     * fallback on failure: guessing this address is a memory-corruption
     * class bug, so a driver that cannot answer fails the open instead. */
	/* Everything from the area probe to the end of LoadModel() runs under
	 * the arena lock (so two opens cannot be handed the same range) AND the
	 * DRP-AI run lock (so no job is running on the NPU while another model is
	 * loaded or the area probe opens the device).  Order: arena, then run;
	 * invoke() only ever takes the run lock. */
	alp_drpai::Arena            &arena = alp_drpai::arena();
	std::unique_lock<std::mutex> arena_lk(arena.mutex());
	std::unique_lock<std::mutex> run_lk(alp_drpai::run_mutex());

	/* One DRP-AI process per board: take the cross-process file lock before
	 * touching the device.  Another process holding it -> ALP_ERR_BUSY.
	 * Handles in this process share the single lock (drpai_arena.h). */
	const int lk = arena.lock_acquire_locked();
	if (lk != 0) {
		return (lk == EWOULDBLOCK || lk == EAGAIN) ? ALP_ERR_BUSY : ALP_ERR_IO;
	}

	uint64_t     mem_base = 0, mem_size = 0;
	alp_status_t mem = _drpai_mem_start(mem_base, mem_size);
	if (mem != ALP_OK) {
		arena.lock_release_locked();
		return mem;
	}

	auto *st = new (std::nothrow) DrpaiState();
	if (st == nullptr) {
		arena.lock_release_locked();
		return ALP_ERR_NOMEM;
	}

	/* Failure exit: tear the runtime down while still holding the run lock
	 * (its dtor talks to the device), then drop both locks and remove the
	 * staging dir. */
	auto fail = [&](alp_status_t rc) {
		const std::string dir = st->staged_dir;
		delete st; /* tears down the runtime before we remove the dir */
		arena.lock_release_locked();
		run_lk.unlock();
		arena_lk.unlock();
		_rm_rf(dir);
		return rc;
	};

	/* Stage the tar out to a private dir; the .dat object files land flat. */
	alp_status_t stage = _stage_drpai_blob(cfg->model_data, cfg->model_size, st->staged_dir);
	if (stage != ALP_OK) {
		return fail(stage);
	}
	st->model_dir = st->staged_dir;

	/* Each handle gets its own range of the arena: every handle would
	 * otherwise load at the base and overwrite the previous one. */
	uint64_t mem_start = 0;
	if (!arena.begin(mem_base, mem_size, mem_start)) {
		return fail(ALP_ERR_NOMEM); /* arena full: close a model first */
	}

	/* LoadModel returns false on a missing/corrupt object dir or a DRP-AI
     * memory-mapping failure. */
	if (!st->runtime.LoadModel(st->model_dir, mem_start)) {
		return fail(ALP_ERR_IO);
	}
	/* GetLastAddress() is the absolute end of the model (0 for a CPU-only
	 * model that used no DRP-AI memory; see drpai_arena.h); the next handle
	 * starts after it.  A model that overruns the arena (minus the
	 * pre-processing reserve) is refused -- its tail would be outside the
	 * carve-out. */
	if (!arena.commit(mem_start, st->runtime.GetLastAddress(), st->arena_range)) {
		return fail(ALP_ERR_NOMEM);
	}
	run_lk.unlock();
	arena_lk.unlock();

	st->in_info  = st->runtime.GetInputInfo();
	st->out_info = st->runtime.GetOutputInfo();

	/* Stage one SDK-owned buffer per input tensor, sized from the
     * wrapper-reported byte size. */
	st->input_bufs.resize(st->in_info.size());
	for (size_t i = 0; i < st->in_info.size(); ++i) {
		st->input_bufs[i].resize(std::get<1>(st->in_info[i]));
	}

	/* Best-effort rank/shape recovery (issue #1635): the MERA wrapper
     * itself exposes no per-tensor shape (see the file header), but
     * deploy.json -- already sitting in st->model_dir -- does.  Anything
     * that doesn't resolve cleanly just leaves the all-empty default
     * below in place, so get_input()/get_output() keep today's rank == 0
     * behaviour exactly as if this block never ran. */
	st->in_shapes.assign(st->in_info.size(), std::vector<uint16_t>());
	st->out_shapes.assign(st->out_info.size(), std::vector<uint16_t>());
	DeployShapes ds;
	if (_drpai_parse_deploy_shapes(st->model_dir, ds)) {
		/* Correlate by node name first -- deploy.json's placeholder node
         * name should match GetInputInfo()'s reported name; positional
         * fallback rules are correlate_input_shapes()'s (drpai_deploy_shapes.h)
         * to keep them unit-testable without the RUHMI/DRP-AI TVM sysroot. */
		std::vector<std::string> in_names;
		in_names.reserve(st->in_info.size());
		for (auto &info : st->in_info) {
			in_names.push_back(std::get<0>(info));
		}
		correlate_input_shapes(in_names, ds, st->in_shapes);
		/* Outputs carry no name to correlate on; deploy.json's `heads`
         * order already matches GetOutput(idx) order (see
         * _drpai_parse_deploy_shapes()'s comment), so correlate
         * positionally, again only when the counts agree. */
		if (ds.output_shapes.size() == st->out_info.size()) {
			st->out_shapes = ds.output_shapes;
		}
	}

	h->be_state = st;
	return ALP_OK;
}

extern "C" std::size_t alp_inference_drpai_num_inputs(struct alp_inference *h_)
{
	auto *h  = h_;
	auto *st = static_cast<DrpaiState *>(h->be_state);
	return (st != nullptr) ? st->in_info.size() : 0u;
}

extern "C" std::size_t alp_inference_drpai_num_outputs(struct alp_inference *h_)
{
	auto *h  = h_;
	auto *st = static_cast<DrpaiState *>(h->be_state);
	return (st != nullptr) ? st->out_info.size() : 0u;
}

extern "C" alp_status_t alp_inference_drpai_get_input(struct alp_inference   *h_,
                                                      std::size_t             index,
                                                      alp_inference_tensor_t *out)
{
	auto *h  = h_;
	auto *st = static_cast<DrpaiState *>(h->be_state);
	if (st == nullptr) {
		return ALP_ERR_NOT_READY;
	}
	if (index >= st->in_info.size()) {
		return ALP_ERR_OUT_OF_RANGE;
	}

	/* Hand back the SDK-owned staging buffer; the app fills it before
     * invoke().  rank/shape come from deploy.json, resolved once at
     * open() (see _drpai_parse_deploy_shapes()) -- the MERA wrapper
     * itself does not expose per-input shape via the public surface.
     * size_bytes is authoritative for buffer sizing either way; an empty
     * in_shapes[index] (file missing/unreadable, rank > 4 -- issue
     * #1729, or a count mismatch) leaves rank 0, same as before this
     * backend read deploy.json at all. */
	const std::vector<uint16_t> &shape = st->in_shapes[index];

	out->data       = st->input_bufs[index].data();
	out->size_bytes = std::get<1>(st->in_info[index]);
	out->dtype      = mera_dtype_to_alp(std::get<2>(st->in_info[index]));
	out->rank       = static_cast<uint8_t>(shape.size());
	for (size_t i = 0; i < shape.size(); ++i) {
		out->shape[i] = shape[i];
	}
	for (size_t i = shape.size(); i < 4; ++i) {
		out->shape[i] = 0;
	}
	out->scale      = 1.0f;
	out->zero_point = 0;
	return ALP_OK;
}

extern "C" alp_status_t alp_inference_drpai_get_output(struct alp_inference   *h_,
                                                       std::size_t             index,
                                                       alp_inference_tensor_t *out)
{
	auto *h  = h_;
	auto *st = static_cast<DrpaiState *>(h->be_state);
	if (st == nullptr) {
		return ALP_ERR_NOT_READY;
	}
	if (index >= st->out_info.size()) {
		return ALP_ERR_OUT_OF_RANGE;
	}

	/* GetOutput(idx) -> (dtype, data ptr, elem_count).  Before the first
     * Run() the wrapper returns its zero-initialised output area; after
     * Run() it points at the live result buffer.  The size reported is the
     * one describing THAT buffer (elem_count * dtype size); the
     * GetOutputInfo() size is only the fallback for OTHER, whose element
     * size is unknown.  A live buffer smaller than GetOutputInfo() claims
     * is a runtime/model mismatch -- fail rather than let a caller walk
     * size_bytes past the end of it. */
	InOutDataType dtype;
	void         *data               = nullptr;
	int64_t       num_elems          = 0;
	std::tie(dtype, data, num_elems) = st->runtime.GetOutput(static_cast<int>(index));

	/* rank/shape: same deploy.json-derived, open()-time-resolved source
     * and same fail-safe (empty == rank 0) as get_input() above. */
	const std::vector<uint16_t> &shape      = st->out_shapes[index];
	const std::size_t            info_bytes = std::get<1>(st->out_info[index]);
	std::size_t                  elem_bytes = 0;
	switch (dtype) {
	case InOutDataType::FLOAT32:
	case InOutDataType::INT32:
		elem_bytes = 4u;
		break;
	case InOutDataType::FLOAT16:
		elem_bytes = 2u;
		break;
	case InOutDataType::INT64:
		elem_bytes = 8u;
		break;
	case InOutDataType::OTHER:
	default:
		break;
	}
	std::size_t live_bytes = info_bytes;
	if (elem_bytes != 0u) {
		if (num_elems < 0) {
			return ALP_ERR_IO;
		}
		live_bytes = static_cast<std::size_t>(num_elems) * elem_bytes;
		if (live_bytes < info_bytes) {
			return ALP_ERR_IO;
		}
	}

	out->data       = data;
	out->size_bytes = live_bytes;
	out->dtype      = mera_dtype_to_alp(dtype);
	out->rank       = static_cast<uint8_t>(shape.size());
	for (size_t i = 0; i < shape.size(); ++i) {
		out->shape[i] = shape[i];
	}
	for (size_t i = shape.size(); i < 4; ++i) {
		out->shape[i] = 0;
	}
	out->scale      = 1.0f;
	out->zero_point = 0;
	return ALP_OK;
}

extern "C" alp_status_t alp_inference_drpai_invoke(struct alp_inference *h_)
{
	auto *h  = h_;
	auto *st = static_cast<DrpaiState *>(h->be_state);
	if (st == nullptr) {
		return ALP_ERR_NOT_READY;
	}

	/* The NPU runs one job at a time and the driver rejects a concurrent
	 * DRPAI_START (-EBUSY) rather than queueing it: hold one process-wide
	 * lock across SetInput + Run so handles on other threads wait. */
	std::lock_guard<std::mutex> run_lk(alp_drpai::run_mutex());

	/* Push each SDK-owned input into the runtime.  SetInput is overloaded
     * on fp32 vs fp16; pick by the reported input dtype so fp16 models
     * route through the uint16_t overload (raw half-float bytes). */
	for (size_t i = 0; i < st->input_bufs.size(); ++i) {
		const InOutDataType dt  = std::get<2>(st->in_info[i]);
		const void         *buf = st->input_bufs[i].data();
		if (dt == InOutDataType::FLOAT16) {
			st->runtime.SetInput(static_cast<int>(i), static_cast<const uint16_t *>(buf));
		} else {
			st->runtime.SetInput(static_cast<int>(i), static_cast<const float *>(buf));
		}
	}

	/* Run() is void and blocks until DRP-AI completes; it reports
     * hard faults via its own logging/abort path, not a return code.
	 * A failed job therefore CANNOT be detected here: this returns ALP_OK
	 * even if the driver rejected DRPAI_START or timed out.  A driver status
	 * query is not a cheap fix -- it needs another open() of /dev/drpai0,
	 * which takes the driver semaphore (up to 1000 ms) and the shared-memory
	 * lock and can itself fail -- so none is made.  Callers must sanity-check
	 * outputs (see <alp/inference.h> and docs/bring-up-drpai-v2n.md). */
	st->runtime.Run();
	return ALP_OK;
}

extern "C" void alp_inference_drpai_close(struct alp_inference *h_)
{
	auto *h  = h_;
	auto *st = static_cast<DrpaiState *>(h->be_state);
	if (st == nullptr) {
		return;
	}
	/* MeraDrpRuntimeWrapper owns its DRP-AI mappings + releases them in
     * its dtor (unique_ptr<Impl>); deleting the state tears it down.  Remove
     * the staging dir AFTER the runtime is gone (it may hold the dir open). */
	const std::string      dir   = st->staged_dir;
	const alp_drpai::Range range = st->arena_range;
	{
		/* The runtime's teardown talks to the device: not while another
		 * handle's job is running. */
		std::lock_guard<std::mutex> run_lk(alp_drpai::run_mutex());
		delete st;
	}
	/* Not nested inside the run lock: arena -> run is the only order. */
	alp_drpai::arena().release(range);
	alp_drpai::arena().lock_release(); /* the process lock goes with the last handle */
	_rm_rf(dir);
	h->be_state = nullptr;
}
