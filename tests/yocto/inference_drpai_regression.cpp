/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression + compile coverage for src/yocto/inference_drpai.cpp
 * (issue #1747: alongside inference_ort.cpp and inference_deepx.cpp,
 * this backend was compiled by NO PR gate -- all three default OFF, so a
 * change to any of them could ship broken straight into a release).
 *
 * The EdgeCortix MERA2 / DRP-AI TVM runtime is Renesas/EdgeCortix
 * account-gated and only exists on the RZ/V Yocto SDK sysroot, not in
 * CI -- same constraint inference_ort_regression.cpp and
 * inference_deepx_regression.cpp already solve for ORT/DEEPX. This test
 * applies the same technique: compile src/yocto/inference_drpai.cpp
 * directly against tests/yocto/fakes/drpai/, a clean-room stand-in for
 * MeraDrpRuntimeWrapper.h (see that file's own comment) plus a fake
 * linux/drpai.h for the DRP-AI driver uapi struct/ioctl it also needs.
 *
 * Two kinds of test below.  The device-absent tests run against the
 * default fake (no /dev/drpai0).  The multi-model tests (arena
 * placement, serialised Run()) flip drpai_test::g_device_present so the
 * fake linux/drpai.h answers the area ioctl and open() reaches the fake
 * MeraDrpRuntimeWrapper; they need a `tar` that accepts an empty
 * archive (open() extracts the blob with `tar -xf -`).
 *
 * Coverage of the device-absent path: inference_drpai.cpp's open() resolves the DRP-AI
 * reserved-memory arena via a REAL `::open("/dev/drpai0", O_RDWR)` +
 * `::ioctl(..., DRPAI_GET_DRPAI_AREA, ...)` (see its own
 * "_drpai_mem_start" doc comment) -- a genuine host syscall, not
 * something a header fake can intercept. No CI runner (and no dev
 * host) has /dev/drpai0, so that call always fails with ENOENT and
 * open() always returns ALP_ERR_IO before ever constructing a
 * MeraDrpRuntimeWrapper, regardless of what the fake class does. That
 * is not a gap in this test: "the device is absent" -> ALP_ERR_IO,
 * not a crash or a guess, IS the documented contract
 * (_drpai_errno_to_status's own comment: "ENOENT ... is what lets a
 * caller tell 'no DRP-AI on this board' from 'busy, retry'"), and it is
 * exactly what these tests exercise for real, with no mocking needed.
 * What these tests do NOT reach: LoadModel()/GetInputInfo()/Run() and
 * the tar-staging path, all of which sit behind that same device probe.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_inference_drpai_regression
 *   ctest --test-dir build -R alp_test_inference_drpai_regression
 */

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

extern "C" {
#include "alp/inference.h"
}

#include "drpai_arena.h"
#include "drpai_test_seam.h"
#include "inference_handle_internal.h"
#include "test_assert.h"

/* Forward declarations -- mirrors src/yocto/inference_yocto.c's own
 * #if defined(ALP_SDK_USE_DRPAI_V2N) block; this test links
 * inference_drpai.cpp directly rather than through the dispatcher, so it
 * declares the same hook prototypes itself. */
extern "C" {
alp_status_t alp_inference_drpai_open(struct alp_inference *h, const alp_inference_config_t *cfg);
std::size_t  alp_inference_drpai_num_inputs(struct alp_inference *h);
std::size_t  alp_inference_drpai_num_outputs(struct alp_inference *h);
alp_status_t alp_inference_drpai_get_input(struct alp_inference   *h,
                                           std::size_t             index,
                                           alp_inference_tensor_t *out);
alp_status_t alp_inference_drpai_get_output(struct alp_inference   *h,
                                            std::size_t             index,
                                            alp_inference_tensor_t *out);
alp_status_t alp_inference_drpai_invoke(struct alp_inference *h);
void         alp_inference_drpai_close(struct alp_inference *h);
}

namespace
{

const uint8_t          k_dummy_model[4] = { 'D', 'R', 'P', 'a' };
alp_inference_config_t base_cfg()
{
	alp_inference_config_t cfg = {};
	cfg.model_data             = k_dummy_model;
	cfg.model_size             = sizeof(k_dummy_model);
	cfg.format                 = ALP_INFERENCE_MODEL_DRPAI;
	cfg.backend                = ALP_INFERENCE_BACKEND_DRPAI;
	return cfg;
}

/* Test 1: open() rejects a NULL model_data before touching the device
 * (the earliest guard in the function -- host-independent). */
void test_open_rejects_null_model_data()
{
	struct alp_inference   h   = {};
	alp_inference_config_t cfg = base_cfg();
	cfg.model_data             = nullptr;

	alp_status_t rc = alp_inference_drpai_open(&h, &cfg);

	ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
	ALP_ASSERT_NULL(h.be_state);
}

/* Test 1b: open() rejects a zero model_size the same way. */
void test_open_rejects_zero_model_size()
{
	struct alp_inference   h   = {};
	alp_inference_config_t cfg = base_cfg();
	cfg.model_size             = 0;

	alp_status_t rc = alp_inference_drpai_open(&h, &cfg);

	ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
	ALP_ASSERT_NULL(h.be_state);
}

/* Test 2: with a well-formed cfg, open() still fails cleanly -- no
 * /dev/drpai0 exists on this host, so _drpai_mem_start() hits ENOENT and
 * open() returns ALP_ERR_IO without allocating be_state.  This is the
 * "no DRP-AI on this board" contract from _drpai_errno_to_status's own
 * doc comment, exercised for real (no fake needed for this path). */
void test_open_fails_cleanly_when_device_absent()
{
	struct alp_inference   h   = {};
	alp_inference_config_t cfg = base_cfg();

	alp_status_t rc = alp_inference_drpai_open(&h, &cfg);

	ALP_ASSERT_EQ_INT(rc, ALP_ERR_IO);
	ALP_ASSERT_NULL(h.be_state);
}

/* An empty tar archive (two zero blocks): what open() stages for the fake. */
const std::vector<uint8_t> k_empty_tar(1024, 0);

alp_inference_config_t tar_cfg()
{
	alp_inference_config_t cfg = base_cfg();
	cfg.model_data             = k_empty_tar.data();
	cfg.model_size             = k_empty_tar.size();
	return cfg;
}

void fake_device(uint64_t model_bytes)
{
	drpai_test::g_device_present = true;
	drpai_test::g_model_bytes    = model_bytes;
	drpai_test::g_cpu_only       = false;
	drpai_test::g_load_starts.clear();
	drpai_test::g_max_in_flight  = 0;
	drpai_test::g_in_flight      = 0;
	drpai_test::g_runs           = 0;
	drpai_test::g_run_block      = false;
	drpai_test::g_overlap_events = 0;
}

constexpr uint64_t kMiB = 1ull << 20;

/* Test 2b: open handles get DISJOINT, ordered, 16 MiB-aligned ranges, and a
 * handle that does not fit is refused with ALP_ERR_NOMEM.  Before the arena
 * allocator every handle loaded at the arena base and overwrote the previous
 * one. */
void test_handles_get_disjoint_arena_ranges()
{
	fake_device(64 * kMiB);
	alp_inference_config_t cfg = tar_cfg();
	struct alp_inference   a = {}, b = {};

	ALP_ASSERT_EQ_INT(alp_inference_drpai_open(&a, &cfg), ALP_OK);
	ALP_ASSERT_EQ_INT(alp_inference_drpai_open(&b, &cfg), ALP_OK);
	ALP_ASSERT_EQ_INT((int)drpai_test::g_load_starts.size(), 2);
	ALP_ASSERT_TRUE(drpai_test::g_load_starts[0] == drpai_test::g_area_base);
	ALP_ASSERT_TRUE(drpai_test::g_load_starts[1] == drpai_test::g_area_base + 64 * kMiB);
	ALP_ASSERT_TRUE(drpai_test::g_load_starts[1] % (16 * kMiB) == 0);

	/* 512 MiB - 32 MiB pre-processing reserve = 480 MiB usable; 2 x 64 MiB
	 * used, so 5 more 64 MiB models fit and the 6th does not. */
	struct alp_inference more[6] = {};
	int                  opened  = 0;
	alp_status_t         last    = ALP_OK;
	for (auto &h : more) {
		last = alp_inference_drpai_open(&h, &cfg);
		if (last != ALP_OK) {
			break;
		}
		++opened;
	}
	ALP_ASSERT_EQ_INT(opened, 5);
	ALP_ASSERT_EQ_INT(last, ALP_ERR_NOMEM);
	ALP_ASSERT_NULL(more[opened].be_state); /* failed open left no state */

	for (int i = opened - 1; i >= 0; --i) {
		alp_inference_drpai_close(&more[i]);
	}
	alp_inference_drpai_close(&b);
	alp_inference_drpai_close(&a);
	drpai_test::g_device_present = false;
}

/* Test 2c: a single model larger than the usable arena is refused. */
void test_oversized_model_is_refused()
{
	fake_device(496 * kMiB); /* > 480 MiB usable */
	alp_inference_config_t cfg = tar_cfg();
	struct alp_inference   h   = {};
	ALP_ASSERT_EQ_INT(alp_inference_drpai_open(&h, &cfg), ALP_ERR_NOMEM);
	ALP_ASSERT_NULL(h.be_state);
	drpai_test::g_device_present = false;
}

/* Test 2e: ranges are reclaimed.  LIFO close then reopen reuses the space,
 * and the "swap B while A stays open" cycle repeated far more often than the
 * arena could hold never hits NOMEM. */
void test_closed_ranges_are_reclaimed()
{
	fake_device(64 * kMiB);
	alp_inference_config_t cfg = tar_cfg();
	struct alp_inference   a = {}, b = {}, c = {};

	ALP_ASSERT_EQ_INT(alp_inference_drpai_open(&a, &cfg), ALP_OK);
	ALP_ASSERT_EQ_INT(alp_inference_drpai_open(&b, &cfg), ALP_OK);
	alp_inference_drpai_close(&b); /* LIFO */
	drpai_test::g_load_starts.clear();
	ALP_ASSERT_EQ_INT(alp_inference_drpai_open(&c, &cfg), ALP_OK);
	ALP_ASSERT_TRUE(drpai_test::g_load_starts[0] == drpai_test::g_area_base + 64 * kMiB);
	alp_inference_drpai_close(&c);

	for (int i = 0; i < 40; ++i) { /* 40 x 64 MiB >> 480 MiB */
		struct alp_inference x = {};
		ALP_ASSERT_EQ_INT(alp_inference_drpai_open(&x, &cfg), ALP_OK);
		alp_inference_drpai_close(&x);
	}
	/* Closing the LOWER handle first leaves the upper range live: the cursor
	 * stays above it, then falls back once it closes too. */
	ALP_ASSERT_EQ_INT(alp_inference_drpai_open(&b, &cfg), ALP_OK);
	alp_inference_drpai_close(&a);
	alp_inference_drpai_close(&b);
	drpai_test::g_load_starts.clear();
	ALP_ASSERT_EQ_INT(alp_inference_drpai_open(&a, &cfg), ALP_OK);
	ALP_ASSERT_TRUE(drpai_test::g_load_starts[0] == drpai_test::g_area_base);
	alp_inference_drpai_close(&a);
	drpai_test::g_device_present = false;
}

/* Test 2f: a CPU-only model (GetLastAddress() == 0) opens and uses no arena. */
void test_cpu_only_model_uses_no_arena()
{
	fake_device(64 * kMiB);
	drpai_test::g_cpu_only     = true;
	alp_inference_config_t cfg = tar_cfg();
	struct alp_inference   a = {}, b = {};
	ALP_ASSERT_EQ_INT(alp_inference_drpai_open(&a, &cfg), ALP_OK);
	drpai_test::g_cpu_only = false;
	ALP_ASSERT_EQ_INT(alp_inference_drpai_open(&b, &cfg), ALP_OK);
	ALP_ASSERT_TRUE(drpai_test::g_load_starts[1] == drpai_test::g_area_base);
	alp_inference_drpai_close(&b);
	alp_inference_drpai_close(&a);
	drpai_test::g_device_present = false;
}

/* Test 2g: a driver area that changes under live handles is refused. */
void test_area_change_with_live_handle_is_refused()
{
	fake_device(64 * kMiB);
	alp_inference_config_t cfg = tar_cfg();
	struct alp_inference   a = {}, b = {};
	ALP_ASSERT_EQ_INT(alp_inference_drpai_open(&a, &cfg), ALP_OK);
	const uint64_t old_base = drpai_test::g_area_base;
	drpai_test::g_area_base = old_base + 0x10000000ull;
	ALP_ASSERT_EQ_INT(alp_inference_drpai_open(&b, &cfg), ALP_ERR_NOMEM);
	drpai_test::g_area_base = old_base;
	alp_inference_drpai_close(&a);
	drpai_test::g_device_present = false;
}

/* The lock file the SDK used: the first of its two locations that exists.
 * Returns an fd on it (or -1). */
int open_lock_file()
{
	for (const char *path : { alp_drpai::kLockPathPrimary, alp_drpai::kLockPathFallback }) {
		int fd = ::open(path, O_RDWR | O_CLOEXEC);
		if (fd >= 0) {
			return fd;
		}
	}
	return -1;
}

/* True when some other open file description holds the lock (what a second
 * process would see): a non-blocking flock() on our own fd fails EWOULDBLOCK. */
bool lock_is_held_by_others()
{
	int fd = open_lock_file();
	if (fd < 0) {
		return false;
	}
	const bool held = ::flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
	::close(fd); /* also drops the lock if we got it */
	return held;
}

/* Test 2i: one DRP-AI process per board.  The first handle takes a file
 * lock that stays until the LAST handle in the process closes; handles in
 * the same process share it; a lock held elsewhere (a second process -- a
 * flock on another file description behaves the same as a fork()ed child)
 * makes open() return ALP_ERR_BUSY and leaves nothing behind. */
void test_one_drpai_process_per_board()
{
	fake_device(16 * kMiB);
	alp_inference_config_t cfg = tar_cfg();
	struct alp_inference   a = {}, b = {}, other = {};

	ALP_ASSERT_EQ_INT(alp_inference_drpai_open(&a, &cfg), ALP_OK);
	ALP_ASSERT_TRUE(lock_is_held_by_others());
	ALP_ASSERT_EQ_INT(alp_inference_drpai_open(&b, &cfg), ALP_OK); /* same process: fine */
	alp_inference_drpai_close(&a);
	ALP_ASSERT_TRUE(lock_is_held_by_others()); /* b still open */
	alp_inference_drpai_close(&b);
	ALP_ASSERT_TRUE(!lock_is_held_by_others()); /* last close released it */

	/* "Another process" holds the board. */
	int fd = open_lock_file();
	ALP_ASSERT_TRUE(fd >= 0);
	ALP_ASSERT_EQ_INT(::flock(fd, LOCK_EX | LOCK_NB), 0);
	ALP_ASSERT_EQ_INT(alp_inference_drpai_open(&other, &cfg), ALP_ERR_BUSY);
	ALP_ASSERT_NULL(other.be_state);
	::close(fd);

	/* The refused open took nothing: once the other holder is gone it works. */
	ALP_ASSERT_EQ_INT(alp_inference_drpai_open(&other, &cfg), ALP_OK);
	alp_inference_drpai_close(&other);
	ALP_ASSERT_TRUE(!lock_is_held_by_others());
	drpai_test::g_device_present = false;
}

/* Test 2d: invoke() from several threads on several handles never has two
 * Run()s in flight at once (the fake sleeps inside Run() to widen the
 * window; without the process-wide lock the high-water mark reaches >1). */
void test_concurrent_invokes_are_serialised()
{
	fake_device(16 * kMiB);
	alp_inference_config_t cfg  = tar_cfg();
	struct alp_inference   h[3] = {};
	for (auto &x : h) {
		ALP_ASSERT_EQ_INT(alp_inference_drpai_open(&x, &cfg), ALP_OK);
	}

	std::vector<std::thread> th;
	for (auto &x : h) {
		th.emplace_back([&x]() {
			for (int i = 0; i < 10; ++i) {
				(void)alp_inference_drpai_invoke(&x);
			}
		});
	}
	for (auto &t : th) {
		t.join();
	}
	ALP_ASSERT_EQ_INT(drpai_test::g_runs.load(), 30);
	ALP_ASSERT_EQ_INT(drpai_test::g_max_in_flight.load(), 1);

	for (auto &x : h) {
		alp_inference_drpai_close(&x);
	}
	drpai_test::g_device_present = false;
}

/* Test 2h: open() and close() of one handle wait for another handle's job:
 * the model load and the runtime teardown touch the device, which runs one
 * job at a time.  A job is held open on handle A; B is opened and C closed
 * from other threads and neither may finish (nor touch the device) until
 * A's Run() returns. */
void test_open_and_close_wait_for_a_running_job()
{
	fake_device(64 * kMiB);
	alp_inference_config_t cfg = tar_cfg();
	struct alp_inference   a = {}, b = {}, c = {};
	ALP_ASSERT_EQ_INT(alp_inference_drpai_open(&a, &cfg), ALP_OK);
	ALP_ASSERT_EQ_INT(alp_inference_drpai_open(&c, &cfg), ALP_OK);

	drpai_test::g_run_block = true;
	std::thread run_a([&a]() { (void)alp_inference_drpai_invoke(&a); });
	while (drpai_test::g_in_flight.load() == 0) { /* A is inside Run() */
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	std::atomic<int> opened{ 0 }, closed{ 0 };
	alp_status_t     open_rc = ALP_ERR_IO;
	std::thread      open_b([&]() {
		open_rc = alp_inference_drpai_open(&b, &cfg);
		opened  = 1;
	});
	std::thread      close_c([&]() {
		alp_inference_drpai_close(&c);
		closed = 1;
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	ALP_ASSERT_EQ_INT(opened.load(), 0); /* still waiting for A's job */
	ALP_ASSERT_EQ_INT(closed.load(), 0);

	drpai_test::g_run_block = false;
	run_a.join();
	open_b.join();
	close_c.join();
	ALP_ASSERT_EQ_INT(open_rc, ALP_OK);
	ALP_ASSERT_EQ_INT(drpai_test::g_overlap_events.load(), 0);

	alp_inference_drpai_close(&b);
	alp_inference_drpai_close(&a);
	drpai_test::g_device_present = false;
}

/* Test 3: num_inputs()/num_outputs() on a never-opened handle report 0,
 * not a crash on a NULL be_state. */
void test_num_inputs_outputs_zero_when_not_open()
{
	struct alp_inference h = {};

	ALP_ASSERT_EQ_INT(alp_inference_drpai_num_inputs(&h), 0);
	ALP_ASSERT_EQ_INT(alp_inference_drpai_num_outputs(&h), 0);
}

/* Test 4: get_input()/get_output() on a never-opened handle report
 * ALP_ERR_NOT_READY, not a NULL-deref. */
void test_get_input_output_not_ready_when_not_open()
{
	struct alp_inference   h   = {};
	alp_inference_tensor_t out = {};

	ALP_ASSERT_EQ_INT(alp_inference_drpai_get_input(&h, 0, &out), ALP_ERR_NOT_READY);
	ALP_ASSERT_EQ_INT(alp_inference_drpai_get_output(&h, 0, &out), ALP_ERR_NOT_READY);
}

/* Test 5: invoke() on a never-opened handle reports ALP_ERR_NOT_READY. */
void test_invoke_not_ready_when_not_open()
{
	struct alp_inference h = {};

	ALP_ASSERT_EQ_INT(alp_inference_drpai_invoke(&h), ALP_ERR_NOT_READY);
}

/* Test 6: close() on a never-opened (be_state == NULL) handle is a
 * no-op, not a crash. */
void test_close_on_null_state_is_noop()
{
	struct alp_inference h = {};

	alp_inference_drpai_close(&h);

	ALP_ASSERT_NULL(h.be_state);
}

} /* namespace */

int main(void)
{
	test_open_rejects_null_model_data();
	test_open_rejects_zero_model_size();
	test_open_fails_cleanly_when_device_absent();
	test_handles_get_disjoint_arena_ranges();
	test_oversized_model_is_refused();
	test_closed_ranges_are_reclaimed();
	test_cpu_only_model_uses_no_arena();
	test_area_change_with_live_handle_is_refused();
	test_concurrent_invokes_are_serialised();
	test_open_and_close_wait_for_a_running_job();
	test_one_drpai_process_per_board();
	test_num_inputs_outputs_zero_when_not_open();
	test_get_input_output_not_ready_when_not_open();
	test_invoke_not_ready_when_not_open();
	test_close_on_null_state_is_noop();

	ALP_TEST_SUMMARY();
}
