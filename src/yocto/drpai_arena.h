/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Process-wide placement of DRP-AI models inside the reserved working-memory
 * arena, the lock that serialises DRP-AI work, and the one-process-per-board
 * file lock.  Split out of
 * inference_drpai.cpp -- like drpai_deploy_shapes.h -- because none of it
 * needs MeraDrpRuntimeWrapper.h or the driver; tests/yocto/
 * inference_drpai_regression.cpp exercises it through inference_drpai.cpp
 * against the fakes in tests/yocto/fakes/drpai/.
 *
 * Why it exists: LoadModel(dir, start_address) writes the model's DRP
 * descriptors, weights and I/O buffers at start_address, and every
 * DRPAI_GET_DRPAI_AREA on a fresh fd answers with the same base.  Without a
 * placement policy a second handle loaded at the base and silently overwrote
 * the first.  Renesas' own two-model sample gives each model its own range
 * and runs the models one after the other; this does the same.
 *
 * Policy: each model starts at the first aligned address at or after the end
 * of the highest live range.  GetLastAddress() of the Renesas wrapper
 * (rzv_drp-ai_tvm Release-2026-04-17, apps/MeraDrpRuntimeWrapper.h,
 * `uint64_t GetLastAddress()`) returns the ABSOLUTE end address of the model
 * just loaded, or 0 for a CPU-only model that used no DRP-AI memory; Renesas'
 * own tutorial aligns the next start to 16 MiB, so that is the alignment
 * here.  When a handle closes, its range is dropped and the cursor falls back
 * to the highest end among the handles still open (the arena base if none),
 * so a LIFO close, or closing a model and loading another, reuses the space.
 * A hole below a still-open higher range is not reused.  The tail of the
 * arena is kept free for DRP-AI pre-processing objects, which PreRuntime
 * places at the END of the region.
 *
 * One process per board: the placement above is per process, and two
 * processes would each load at the arena base and corrupt each other.  The
 * first DRP-AI handle in a process therefore takes an exclusive, non-blocking
 * flock() on a lock file and keeps it until the last DRP-AI handle in the
 * process closes; another process finds it held and its open() fails with
 * ALP_ERR_BUSY.  Handles inside one process share the one lock.  The file is
 * /run/alp/drpai.lock (directory created 0755 if missing); /run is tmpfs on
 * the target image and root-writable, which is where runtime lock files
 * belong and is cleared on every boot, so a stale file cannot matter (flock
 * state dies with the process anyway).  Only when /run/alp cannot be created
 * or opened (not writable, e.g. an unprivileged host run) does it fall back
 * to /tmp/alp-drpai.lock.
 */
#ifndef ALP_SDK_YOCTO_DRPAI_ARENA_H
#define ALP_SDK_YOCTO_DRPAI_ARENA_H

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace alp_drpai
{

/** Model ranges start on a 16 MiB boundary, as Renesas' own tutorial does. */
constexpr uint64_t kArenaAlign = 16ull << 20;

/** Bytes kept free at the end of the arena for DRP-AI pre-processing.
 *  ponytail: an estimate -- no compiled bundle is available to measure it
 *  (alp-sdk#2236); retune once PreRuntime's real footprint is known. */
constexpr uint64_t kArenaPreprocReserve = 32ull << 20;

/** Lock file locations, in order of preference (see the file comment). */
constexpr const char *kLockDir          = "/run/alp";
constexpr const char *kLockPathPrimary  = "/run/alp/drpai.lock";
constexpr const char *kLockPathFallback = "/tmp/alp-drpai.lock";

/** A model's [start, end) in the arena.  start == end means "uses none"
 *  (CPU-only model); releasing it is a no-op. */
struct Range {
	uint64_t start = 0;
	uint64_t end   = 0;
};

class Arena
{
  public:
	/** Held by open() from begin() until commit() has run, so two concurrent
	 *  opens cannot be handed the same range. */
	std::mutex &mutex()
	{
		return mu_;
	}

	/** Where the next model should be loaded, or false when no room is left
	 *  or the driver's area changed under live handles.  Caller holds
	 *  mutex().  @p base/@p size are the driver's area. */
	bool begin(uint64_t base, uint64_t size, uint64_t &start)
	{
		if (live_.empty()) {
			base_ = base;
			size_ = size;
		} else if (base_ != base || size_ != size) {
			return false;
		}
		start = align_up(cursor_());
		return start < limit_() && start >= base_;
	}

	/** Record that the model loaded at @p start ends at @p last_address (the
	 *  runtime's GetLastAddress(); 0 = CPU-only, uses no arena).  Caller holds
	 *  mutex().
	 *  @return false when the model does not fit below the pre-processing
	 *          reserve (or the runtime reported nonsense); nothing is
	 *          recorded -- the caller unloads the model.  On success @p out
	 *          is what release() must be given back. */
	bool commit(uint64_t start, uint64_t last_address, Range &out)
	{
		if (last_address == 0) {
			out = Range{};
			return true;
		}
		if (last_address <= start || last_address > limit_()) {
			return false;
		}
		out = Range{ start, last_address };
		live_.push_back(out);
		return true;
	}

	/** Drop @p r (a commit() result).  The cursor then falls back to the
	 *  highest end among the remaining ranges. */
	void release(const Range &r)
	{
		if (r.start == r.end) {
			return;
		}
		std::lock_guard<std::mutex> lk(mu_);
		auto it = std::find_if(live_.begin(), live_.end(), [&](const Range &x) {
			return x.start == r.start && x.end == r.end;
		});
		if (it != live_.end()) {
			live_.erase(it);
		}
	}

	/** Take (or share, if this process already holds it) the one-process-per-
	 *  board lock.  Caller holds mutex().  Returns 0, EWOULDBLOCK when another
	 *  process holds it, or another errno.  Every success must be paired with
	 *  one lock_release_locked() / lock_release(). */
	int lock_acquire_locked()
	{
		if (lock_holders_ == 0) {
			int fd = open_lock_file_();
			if (fd < 0) {
				return errno;
			}
			if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
				const int err = errno;
				::close(fd);
				return err;
			}
			lock_fd_ = fd;
		}
		++lock_holders_;
		return 0;
	}

	/** Drop one hold; the file lock goes with the last.  Caller holds mutex(). */
	void lock_release_locked()
	{
		if (lock_holders_ != 0 && --lock_holders_ == 0) {
			::close(lock_fd_); /* closing the fd drops the flock */
			lock_fd_ = -1;
		}
	}

	/** lock_release_locked() for a caller that does not hold mutex(). */
	void lock_release()
	{
		std::lock_guard<std::mutex> lk(mu_);
		lock_release_locked();
	}

	/** Test hook: ranges currently held. */
	size_t live()
	{
		std::lock_guard<std::mutex> lk(mu_);
		return live_.size();
	}

  private:
	/** Open the lock file: /run/alp/drpai.lock, else /tmp/alp-drpai.lock. */
	static int open_lock_file_()
	{
		::mkdir(kLockDir, 0755); /* EEXIST and EACCES both fall through to open() */
		int fd = ::open(kLockPathPrimary, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
		if (fd < 0) {
			fd = ::open(kLockPathFallback, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
		}
		return fd;
	}

	uint64_t cursor_() const
	{
		uint64_t c = base_;
		for (const Range &r : live_) {
			c = std::max(c, r.end);
		}
		return c;
	}
	uint64_t limit_() const
	{
		return size_ > kArenaPreprocReserve ? base_ + size_ - kArenaPreprocReserve : base_;
	}
	static uint64_t align_up(uint64_t v)
	{
		return (v + kArenaAlign - 1u) & ~(kArenaAlign - 1u);
	}

	std::mutex         mu_;
	uint64_t           base_ = 0;
	uint64_t           size_ = 0;
	std::vector<Range> live_;
	int                lock_fd_      = -1;
	unsigned           lock_holders_ = 0;
};

/** The one DRP-AI arena of this process. */
inline Arena &arena()
{
	static Arena a;
	return a;
}

/** Held around SetInput()+Run() AND around every other call that touches the
 *  DRP-AI device: the model load in open() and the runtime teardown in
 *  close().  DRP-AI runs one job at a time and the driver answers a second
 *  DRPAI_START with -EBUSY instead of queueing it; the closed runtime treats
 *  that as a hard failure.  One lock for every handle in the process makes
 *  concurrent users wait their turn.  Lock order: arena().mutex() first,
 *  then run_mutex(); invoke never takes the arena lock.
 *  ponytail: plain mutex, no FIFO fairness; add a ticket lock if a starved
 *  handle is ever measured. */
inline std::mutex &run_mutex()
{
	static std::mutex m;
	return m;
}

} /* namespace alp_drpai */

#endif /* ALP_SDK_YOCTO_DRPAI_ARENA_H */
