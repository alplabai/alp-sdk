/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Process-wide placement of DRP-AI models inside the reserved working-memory
 * arena, plus the lock that serialises DRP-AI jobs.  Split out of
 * inference_drpai.cpp -- like drpai_deploy_shapes.h -- because none of it
 * needs MeraDrpRuntimeWrapper.h or the driver, so
 * tests/yocto/inference_drpai_arena.cpp can exercise the real code on a plain
 * host.
 *
 * Why it exists: LoadModel(dir, start_address) writes the model's DRP
 * descriptors, weights and I/O buffers at start_address, and every
 * DRPAI_GET_DRPAI_AREA on a fresh fd answers with the same base.  Without a
 * placement policy a second handle loaded at the base and silently overwrote
 * the first.  Renesas' own two-model sample gives each model its own range
 * and runs the models one after the other; this does the same.
 *
 * Policy: a bump allocator.  A model starts at the first aligned address after
 * the previous one (the runtime reports where a model ended via
 * GetLastAddress()).  Ranges are not recycled individually; the cursor goes
 * back to the arena base when the last handle closes (the driver resets the
 * NPU on the last close anyway).  The tail of the arena is kept free for
 * DRP-AI pre-processing objects, which PreRuntime places at the END of the
 * region.
 */
#ifndef ALP_SDK_YOCTO_DRPAI_ARENA_H
#define ALP_SDK_YOCTO_DRPAI_ARENA_H

#include <cstdint>
#include <mutex>

namespace alp_drpai
{

/** Model ranges start on a 1 MiB boundary (page-aligned with margin; trivial
 *  against a 512 MiB arena). */
constexpr uint64_t kArenaAlign = 1ull << 20;

/** Bytes kept free at the end of the arena for DRP-AI pre-processing.
 *  ponytail: an estimate -- no compiled bundle is available to measure it
 *  (alp-sdk#2236); retune once PreRuntime's real footprint is known. */
constexpr uint64_t kArenaPreprocReserve = 32ull << 20;

class Arena
{
  public:
	/** Held by open() from begin() until commit()/abort(), so two concurrent
	 *  opens cannot be handed the same range. */
	std::mutex &mutex()
	{
		return mu_;
	}

	/** Where the next model should be loaded, or false when no room is left.
	 *  Caller holds mutex().  @p base/@p size are the driver's area. */
	bool begin(uint64_t base, uint64_t size, uint64_t &start)
	{
		if (live_ == 0 || base_ != base || size_ != size) {
			base_   = base;
			size_   = size;
			cursor_ = base;
		}
		const uint64_t limit = limit_();
		start                = align_up(cursor_);
		return start < limit && start >= base_;
	}

	/** Record that the model loaded at @p start ends at @p last_address (the
	 *  runtime's GetLastAddress()).  Caller holds mutex().
	 *  @return false when the model does not fit below the pre-processing
	 *          reserve (or the runtime reported nonsense); nothing is
	 *          recorded -- the caller unloads the model. */
	bool commit(uint64_t start, uint64_t last_address)
	{
		if (last_address <= start || last_address > limit_()) {
			return false;
		}
		cursor_ = align_up(last_address);
		++live_;
		return true;
	}

	/** One committed model went away.  Rewinds the cursor with the last. */
	void release()
	{
		std::lock_guard<std::mutex> lk(mu_);
		if (live_ != 0 && --live_ == 0) {
			cursor_ = base_;
		}
	}

	/** Test hook. */
	unsigned live()
	{
		std::lock_guard<std::mutex> lk(mu_);
		return live_;
	}

  private:
	uint64_t limit_() const
	{
		return size_ > kArenaPreprocReserve ? base_ + size_ - kArenaPreprocReserve : base_;
	}
	static uint64_t align_up(uint64_t v)
	{
		return (v + kArenaAlign - 1u) & ~(kArenaAlign - 1u);
	}

	std::mutex mu_;
	uint64_t   base_   = 0;
	uint64_t   size_   = 0;
	uint64_t   cursor_ = 0;
	unsigned   live_   = 0;
};

/** The one DRP-AI arena of this process. */
inline Arena &arena()
{
	static Arena a;
	return a;
}

/** Held around SetInput()+Run().  DRP-AI runs one job at a time and the
 *  driver answers a second DRPAI_START with -EBUSY instead of queueing it;
 *  the closed runtime treats that as a hard failure.  One lock for every
 *  handle in the process makes concurrent invokes wait their turn.
 *  ponytail: plain mutex, no FIFO fairness; add a ticket lock if a starved
 *  handle is ever measured. */
inline std::mutex &run_mutex()
{
	static std::mutex m;
	return m;
}

} /* namespace alp_drpai */

#endif /* ALP_SDK_YOCTO_DRPAI_ARENA_H */
