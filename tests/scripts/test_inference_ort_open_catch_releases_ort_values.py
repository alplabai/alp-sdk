# SPDX-License-Identifier: Apache-2.0
"""Regression test for #1494.

`src/yocto/inference_ort.cpp`'s `alp_inference_ort_open()` has THREE teardown
paths that must all release the same set of ORT objects: the `rc != ALP_OK`
early-return, `alp_inference_ort_close()`, and the `catch (...)` handler that
guards the file's C++/C exception boundary. Before this fix the `catch (...)`
handler released only `cpu_mem_info` / `session` / `env` and skipped the
`_release_values(api, st->output_values)` / `_release_values(api,
st->input_values)` pair its two siblings both perform -- leaking every
`OrtValue` already created (e.g. by a `std::bad_alloc` / `std::length_error`
out of `st->output_bufs[i].resize(...)` after the input loop has already
populated `st->input_values`) before the throw.

The three copies have since been deduplicated into one shared `_teardown()`
helper that all three sites call -- the duplication was the root cause of the
drift #1494 fixed. So this test now checks two things: that open()'s
`catch (...)` handler tears down `st` through `_teardown()` rather than a
hand-inlined copy that could drift again, and that `_teardown()` itself
releases both OrtValue vectors before the Env/Session/MemoryInfo trio.

This file can't be compiled/run here (ONNX Runtime is not installed on this
host and `ALP_SDK_USE_ORT_CPU` defaults OFF, matching every other TU in this
directory -- see `docs/verification-status.md`), so this is a structural
check on the source text, in the style of the other `tests/scripts/test_*`
regression tests that parse a source/overlay file directly (e.g.
`test_pwm_led_fade_aen_overlay.py`). It is proven against synthetic
"regressed" corpora, not just the live file, so it cannot pass vacuously if
the catch block's or `_teardown()`'s shape ever drifts.

Run locally:

    python3 -m pytest tests/scripts/test_inference_ort_open_catch_releases_ort_values.py -q
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "src" / "yocto" / "inference_ort.cpp"

_RELEASE_OUTPUT = "_release_values(api, st->output_values);"
_RELEASE_INPUT = "_release_values(api, st->input_values);"
_TEARDOWN_CALL = "_teardown(api, st.get());"


def _brace_body(text: str, open_brace_idx: int) -> str:
    """Return the text strictly between the `{` at `open_brace_idx` and its
    matching `}`, found by brace-counting.
    """
    depth = 1
    i = open_brace_idx + 1
    while depth > 0:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return text[open_brace_idx + 1 : i - 1]


def _open_catch_block(text: str) -> str:
    """Extract the `catch (...) { ... }` body that follows
    `alp_inference_ort_open`'s try block, by brace-counting from the
    `} catch (...) {` that closes that function's try.
    """
    open_idx = text.index("alp_inference_ort_open(")
    catch_idx = text.index("} catch (...) {", open_idx)
    return _brace_body(text, text.index("{", catch_idx))


def _teardown_body(text: str) -> str:
    """Extract the body of the shared `_teardown()` helper that open()'s
    catch, the `rc != ALP_OK` path, and `close()` all call -- see #1494's
    follow-up dedup (the three hand-copied teardowns were the drift surface
    that caused #1494 in the first place).
    """
    start = text.index("_teardown(const OrtApi")
    return _brace_body(text, text.index("{", start))


def _catch_releases_ort_values(text: str) -> bool:
    """True iff open()'s catch (...) handler tears down `st` through the
    shared `_teardown()` helper (rather than a hand-inlined copy that can
    drift, per #1494), and `_teardown()` itself releases both OrtValue
    vectors before the Env/Session/MemoryInfo trio.
    """
    if _TEARDOWN_CALL not in _open_catch_block(text):
        return False
    body = _teardown_body(text)
    if _RELEASE_OUTPUT not in body or _RELEASE_INPUT not in body:
        return False
    if "ReleaseMemoryInfo" not in body:
        raise AssertionError(
            "_teardown() no longer calls ReleaseMemoryInfo -- update this "
            "test's ordering check to match the new teardown shape"
        )
    # Must run before the Env/Session/MemoryInfo trio, matching the
    # `rc != ALP_OK` path and alp_inference_ort_close() -- releasing the
    # values after ReleaseEnv()/ReleaseSession() would still be correct
    # (OrtValue release doesn't depend on them), but a release ordered
    # after a `return`-shaped early exit is exactly the kind of thing a
    # careless partial fix produces, so pin the order too.
    release_mem_idx = body.index("ReleaseMemoryInfo")
    return body.index(_RELEASE_OUTPUT) < release_mem_idx and body.index(_RELEASE_INPUT) < release_mem_idx


# The pre-#1494 shape: the catch handler hand-inlines a teardown of
# cpu_mem_info/session/env only, instead of calling the shared _teardown()
# helper (which itself correctly releases the OrtValue vectors first). This
# is a synthetic reduction of the real function/helper, not a copy of the
# whole TU -- just enough context for the brace-counters above.
_LEAKY_CATCH = """
static void _teardown(const OrtApi *api, OrtState *st)
{
	_release_values(api, st->output_values);
	_release_values(api, st->input_values);
	if (st->cpu_mem_info != nullptr) {
		api->ReleaseMemoryInfo(st->cpu_mem_info);
	}
	if (st->session != nullptr) {
		api->ReleaseSession(st->session);
	}
	if (st->env != nullptr) {
		api->ReleaseEnv(st->env);
	}
}

extern "C" alp_status_t alp_inference_ort_open(struct alp_inference *h_,
                                               const alp_inference_config_t *cfg)
{
	std::unique_ptr<OrtState> st;
	try {
		st = std::make_unique<OrtState>();
		h->be_state = st.release();
		return ALP_OK;
	} catch (...) {
		if (st) {
			if (st->cpu_mem_info != nullptr) {
				api->ReleaseMemoryInfo(st->cpu_mem_info);
			}
			if (st->session != nullptr) {
				api->ReleaseSession(st->session);
			}
			if (st->env != nullptr) {
				api->ReleaseEnv(st->env);
			}
		}
		return ALP_ERR_NOMEM;
	}
}
"""

# The fixed shape (matches the real file): open()'s catch calls the shared
# _teardown() helper, which releases the two OrtValue vectors ahead of the
# Env/Session/MemoryInfo trio.
_FIXED_CATCH = """
static void _teardown(const OrtApi *api, OrtState *st)
{
	_release_values(api, st->output_values);
	_release_values(api, st->input_values);
	if (st->cpu_mem_info != nullptr) {
		api->ReleaseMemoryInfo(st->cpu_mem_info);
	}
	if (st->session != nullptr) {
		api->ReleaseSession(st->session);
	}
	if (st->env != nullptr) {
		api->ReleaseEnv(st->env);
	}
}

extern "C" alp_status_t alp_inference_ort_open(struct alp_inference *h_,
                                               const alp_inference_config_t *cfg)
{
	std::unique_ptr<OrtState> st;
	try {
		st = std::make_unique<OrtState>();
		h->be_state = st.release();
		return ALP_OK;
	} catch (...) {
		if (st) {
			_teardown(api, st.get());
		}
		return ALP_ERR_NOMEM;
	}
}
"""

# A partial/misordered "fix" that must still fail: open() calls the shared
# _teardown() helper (so the presence check above passes), but _teardown()
# itself releases the OrtValue vectors AFTER the Env/Session/MemoryInfo
# trio -- covered here to prove the ordering check inside _teardown_body()
# still bites now that it moved off the catch block.
_MISORDERED_CATCH = _FIXED_CATCH.replace(
    "\t_release_values(api, st->output_values);\n\t_release_values(api, st->input_values);\n"
    "\tif (st->cpu_mem_info != nullptr) {",
    "\tif (st->cpu_mem_info != nullptr) {",
).replace(
    "\tif (st->env != nullptr) {\n\t\tapi->ReleaseEnv(st->env);\n\t}\n}",
    "\tif (st->env != nullptr) {\n\t\tapi->ReleaseEnv(st->env);\n\t}\n"
    "\t_release_values(api, st->output_values);\n\t_release_values(api, st->input_values);\n}",
)


class TestInferenceOrtOpenCatchReleasesValues(unittest.TestCase):
    def test_synthetic_leaky_catch_is_caught(self) -> None:
        self.assertFalse(_catch_releases_ort_values(_LEAKY_CATCH))

    def test_synthetic_fixed_catch_passes(self) -> None:
        self.assertTrue(_catch_releases_ort_values(_FIXED_CATCH))

    def test_misordered_release_is_still_caught(self) -> None:
        self.assertIn(_RELEASE_OUTPUT, _MISORDERED_CATCH)
        self.assertIn(_RELEASE_INPUT, _MISORDERED_CATCH)
        self.assertFalse(_catch_releases_ort_values(_MISORDERED_CATCH))

    def test_live_file_open_catch_releases_ort_values(self) -> None:
        text = SRC.read_text(encoding="utf-8")
        self.assertTrue(
            _catch_releases_ort_values(text),
            "alp_inference_ort_open()'s catch (...) handler must tear down "
            "st via the shared _teardown() helper, and _teardown() must "
            "release st->output_values and st->input_values (via "
            "_release_values()) before ReleaseMemoryInfo/ReleaseSession/"
            "ReleaseEnv, matching the rc != ALP_OK path and "
            "alp_inference_ort_close() -- see #1494.",
        )


if __name__ == "__main__":
    sys.exit(unittest.main())
