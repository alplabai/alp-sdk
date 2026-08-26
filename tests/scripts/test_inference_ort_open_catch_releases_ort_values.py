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

This file can't be compiled/run here (ONNX Runtime is not installed on this
host and `ALP_SDK_USE_ORT_CPU` defaults OFF, matching every other TU in this
directory -- see `docs/verification-status.md`), so this is a structural
check on the source text, in the style of the other `tests/scripts/test_*`
regression tests that parse a source/overlay file directly (e.g.
`test_pwm_led_fade_aen_overlay.py`). It is proven against a synthetic
"regressed" corpus, not just the live file, so it cannot pass vacuously if
the catch block's shape ever drifts.

Run locally:

    python3 -m pytest tests/scripts/test_inference_ort_open_catch_releases_ort_values.py -q
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "src" / "yocto" / "inference_ort.cpp"

_RELEASE_OUTPUT = "_release_values(api, st->output_values);"
_RELEASE_INPUT = "_release_values(api, st->input_values);"


def _open_catch_block(text: str) -> str:
    """Extract the `catch (...) { ... }` body that follows
    `alp_inference_ort_open`'s try block, by brace-counting from the
    `} catch (...) {` that closes that function's try.
    """
    open_idx = text.index("alp_inference_ort_open(")
    catch_idx = text.index("} catch (...) {", open_idx)
    body_start = text.index("{", catch_idx) + 1
    depth = 1
    i = body_start
    while depth > 0:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return text[body_start : i - 1]


def _catch_releases_ort_values(text: str) -> bool:
    """True iff the open() catch handler releases both OrtValue vectors
    before releasing cpu_mem_info/session/env, mirroring the file's other
    two teardown paths.
    """
    block = _open_catch_block(text)
    if _RELEASE_OUTPUT not in block or _RELEASE_INPUT not in block:
        return False
    # Must run before the Env/Session/MemoryInfo trio, matching the
    # `rc != ALP_OK` path and alp_inference_ort_close() -- releasing the
    # values after ReleaseEnv()/ReleaseSession() would still be correct
    # (OrtValue release doesn't depend on them), but a release ordered
    # after a `return`-shaped early exit is exactly the kind of thing a
    # careless partial fix produces, so pin the order too.
    release_mem_idx = block.index("ReleaseMemoryInfo")
    return block.index(_RELEASE_OUTPUT) < release_mem_idx and block.index(_RELEASE_INPUT) < release_mem_idx


# The pre-#1494 shape: the catch handler tears down cpu_mem_info/session/env
# only. This is a synthetic reduction of the real function, not a copy of the
# whole TU -- just enough context for _open_catch_block()'s brace-counter.
_LEAKY_CATCH = """
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

# The fixed shape (matches the real file): the two _release_values() calls
# run first, ahead of the Env/Session/MemoryInfo trio.
_FIXED_CATCH = """
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
		return ALP_ERR_NOMEM;
	}
}
"""

# A partial/misordered "fix" that must still fail: the release calls exist
# but run AFTER the trio, which would run past a `return` in the real
# multi-statement block shape a careless patch could produce -- covered here
# to prove the ordering check, not just the presence check, actually bites.
_MISORDERED_CATCH = _LEAKY_CATCH.replace(
    "\t\t\tapi->ReleaseEnv(st->env);\n\t\t\t}\n\t\t}\n\t\treturn ALP_ERR_NOMEM;",
    "\t\t\tapi->ReleaseEnv(st->env);\n\t\t\t}\n"
    "\t\t\t_release_values(api, st->output_values);\n"
    "\t\t\t_release_values(api, st->input_values);\n"
    "\t\t}\n\t\treturn ALP_ERR_NOMEM;",
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
            "alp_inference_ort_open()'s catch (...) handler must release "
            "st->output_values and st->input_values (via _release_values()) "
            "before ReleaseMemoryInfo/ReleaseSession/ReleaseEnv, matching "
            "the rc != ALP_OK path and alp_inference_ort_close() -- see #1494.",
        )


if __name__ == "__main__":
    sys.exit(unittest.main())
