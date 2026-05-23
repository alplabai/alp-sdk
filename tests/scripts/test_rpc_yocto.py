# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for src/yocto/rpc_yocto.c (Wave 5A of the 2026-05-15
heterogeneous-OS orchestration design).

Two test classes:

* ``TestRpcYoctoStructure`` -- source-level structural tests that
  parse ``rpc_yocto.c`` and assert the function signatures, the
  framing helpers + the synchronous-call call-slot fields are all
  present.  Runs on every host (Linux, macOS, Windows).

* ``TestRpcYoctoLive`` -- skipped on hosts without libmetal +
  librpmsg.  Documents how an end-to-end harness would invoke
  ``alp_rpc_call`` against a mocked ``/dev/rpmsg*`` chardev using
  ``socketpair``; we leave the harness as documentation since
  building the .so on a non-Linux dev box requires the OpenAMP
  user-space libraries (meta-openamp / libopen-amp-dev) which are
  unavailable on Windows.

Run locally::

    python -m pytest tests/scripts/test_rpc_yocto.py -v
"""

from __future__ import annotations

import os
import platform
import re
import shutil
import sys
from pathlib import Path

import pytest


REPO = Path(__file__).resolve().parents[2]
RPC_SRC = REPO / "src" / "yocto" / "rpc_yocto.c"
RPC_HDR = REPO / "include" / "alp" / "rpc.h"
ZEPHYR_SRC = REPO / "src" / "backends" / "rpc" / "zephyr_drv.c"


# ---------------------------------------------------------------------
# Structural tests -- always run, no toolchain needed
# ---------------------------------------------------------------------


@pytest.fixture(scope="module")
def rpc_source() -> str:
    assert RPC_SRC.exists(), f"missing source: {RPC_SRC}"
    return RPC_SRC.read_text(encoding="utf-8")


@pytest.fixture(scope="module")
def zephyr_source() -> str:
    assert ZEPHYR_SRC.exists(), f"missing source: {ZEPHYR_SRC}"
    return ZEPHYR_SRC.read_text(encoding="utf-8")


@pytest.fixture(scope="module")
def header_source() -> str:
    assert RPC_HDR.exists(), f"missing header: {RPC_HDR}"
    return RPC_HDR.read_text(encoding="utf-8")


class TestRpcYoctoStructure:
    """Parse the rpc_yocto.c source and assert the synchronous-call
    implementation has all the moving parts the design + the public
    header contract require.  These tests run on every host so a CI
    runner without libmetal still verifies the implementation didn't
    silently regress to the NOSUPPORT stub."""

    # ----- Public-API surface ---------------------------------------

    @pytest.mark.parametrize(
        "fn",
        [
            "alp_rpc_open",
            "alp_rpc_close",
            "alp_rpc_subscribe",
            "alp_rpc_unsubscribe",
            "alp_rpc_send",
            "alp_rpc_call",
        ],
    )
    def test_public_api_defined(self, rpc_source: str, fn: str) -> None:
        # Look for the function definition (return type + name + opening paren).
        pattern = rf"\b{re.escape(fn)}\s*\("
        assert re.search(pattern, rpc_source) is not None, \
            f"{fn} not found as a function definition in rpc_yocto.c"

    def test_alp_rpc_call_no_longer_nosupport(self, rpc_source: str) -> None:
        """The pre-Wave-5A stub returned ALP_ERR_NOSUPPORT immediately.
        The real implementation must not contain a top-level
        `return ALP_ERR_NOSUPPORT;` inside the Linux branch.

        The fallback NOSUPPORT path at the bottom of the file (for
        non-Linux hosts) is still allowed -- we slice the source at
        the `#else` boundary before checking."""
        # Find the #else that begins the NOSUPPORT fallback.
        m = re.search(r"#else\s+/\*\s*!__linux__", rpc_source)
        assert m is not None, "expected #else fallback block in rpc_yocto.c"
        linux_branch = rpc_source[: m.start()]

        # Locate alp_rpc_call's body inside the Linux branch.
        call_match = re.search(
            r"alp_status_t\s+alp_rpc_call\s*\(.*?\)\s*\{",
            linux_branch,
            flags=re.DOTALL,
        )
        assert call_match is not None, \
            "alp_rpc_call not found in Linux branch of rpc_yocto.c"

        # Walk braces to find the matching close.
        depth = 0
        i = call_match.end() - 1  # the '{' we just matched
        body_end = None
        while i < len(linux_branch):
            c = linux_branch[i]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    body_end = i
                    break
            i += 1
        assert body_end is not None, "could not balance braces"
        body = linux_branch[call_match.end():body_end]

        # The real implementation must NOT have a NOSUPPORT short-circuit.
        bad = re.findall(r"return\s+ALP_ERR_NOSUPPORT", body)
        assert not bad, (
            "alp_rpc_call's Linux branch still returns ALP_ERR_NOSUPPORT; "
            "Wave 5A should have replaced the stub with a real implementation"
        )

    # ----- Header status string updated ----------------------------

    def test_header_no_longer_advertises_nosupport(
        self, rpc_source: str
    ) -> None:
        """The file-header comment used to say
        ``alp_rpc_call: NOSUPPORT (TODO Wave 5)``.  The post-Wave-5A
        comment should describe the actual implementation strategy."""
        # The top comment block spans up to roughly line 70 (past the
        # `#include "alp/rpc.h"` line).  Slice on that boundary.
        head_match = re.search(r'#include\s+"alp/rpc\.h"', rpc_source)
        assert head_match is not None
        header_block = rpc_source[: head_match.start()]

        assert "TODO Wave 5" not in header_block
        # Positive assertions: the new comment must mention the
        # correlation strategy + the call-slot mutex/cond names.
        assert "correlation strategy" in header_block.lower(), \
            "file header should explain the call/response correlation strategy"
        assert "call_mutex" in header_block, \
            "file header should mention the per-channel call_mutex"
        assert "call_cond" in header_block, \
            "file header should mention the per-channel call_cond"

    # ----- Framing parity with Zephyr ------------------------------

    @pytest.mark.parametrize(
        "helper",
        ["fnv1a_32", "frame_build", "frame_parse", "method_valid"],
    )
    def test_framing_helpers_present(
        self, rpc_source: str, helper: str
    ) -> None:
        """The Linux + Zephyr backends share an identical framing
        format.  Any divergence here breaks the wire protocol."""
        assert re.search(rf"\b{helper}\s*\(", rpc_source) is not None, \
            f"framing helper '{helper}' missing from rpc_yocto.c"

    def test_method_max_len_matches_header(
        self, rpc_source: str, header_source: str
    ) -> None:
        """The on-wire method header is bounded by
        ALP_RPC_METHOD_MAX_LEN -- the source must reference the
        same symbol declared in the public header."""
        m = re.search(r"#define\s+ALP_RPC_METHOD_MAX_LEN\s+(\d+)", header_source)
        assert m is not None, "ALP_RPC_METHOD_MAX_LEN not declared in rpc.h"
        # Sanity: the cap is the documented 32 bytes.
        assert int(m.group(1)) == 32
        assert "ALP_RPC_METHOD_MAX_LEN" in rpc_source

    # ----- Call-slot state in the channel struct -------------------

    @pytest.mark.parametrize(
        "field",
        [
            "call_mutex",
            "call_cond",
            "call_method",
            "call_resp_buf",
            "call_resp_cap",
            "call_resp_len",
            "call_result",
            "call_pending",
        ],
    )
    def test_call_slot_fields(self, rpc_source: str, field: str) -> None:
        """Every call-slot field documented in the file header must
        exist in the channel struct."""
        assert re.search(
            rf"\b{field}\b", rpc_source
        ) is not None, f"call-slot field '{field}' missing"

    # ----- Concurrency primitives -----------------------------------

    @pytest.mark.parametrize(
        "primitive",
        [
            "pthread_mutex_init",
            "pthread_mutex_destroy",
            "pthread_cond_init",
            "pthread_cond_destroy",
            "pthread_cond_timedwait",
            "pthread_cond_signal",
            # close-side wake of pending callers
            "pthread_cond_broadcast",
            "clock_gettime",
            "CLOCK_REALTIME",
        ],
    )
    def test_concurrency_primitives(
        self, rpc_source: str, primitive: str
    ) -> None:
        """The synchronous-call wait uses pthread_cond_timedwait against
        an absolute CLOCK_REALTIME deadline; the close path broadcasts
        to wake pending callers."""
        assert primitive in rpc_source, \
            f"expected '{primitive}' in rpc_yocto.c"

    # ----- Error-code coverage --------------------------------------

    @pytest.mark.parametrize(
        "code",
        [
            "ALP_OK",
            "ALP_ERR_INVAL",
            "ALP_ERR_NOT_READY",
            "ALP_ERR_TIMEOUT",
            "ALP_ERR_NOMEM",
            "ALP_ERR_IO",
        ],
    )
    def test_status_codes(self, rpc_source: str, code: str) -> None:
        """Every status code documented for alp_rpc_call in rpc.h
        must appear somewhere in rpc_yocto.c."""
        assert code in rpc_source

    def test_call_serialises_via_tx_mutex(self, rpc_source: str) -> None:
        """The header docs (`@note Concurrent calls on the same
        channel from multiple threads are serialised by the SDK`)
        require alp_rpc_call to take tx_mutex end-to-end."""
        # Find alp_rpc_call's body & confirm tx_mutex is locked + unlocked.
        m = re.search(
            r"alp_status_t\s+alp_rpc_call\s*\(.*?\)\s*\{(.*?)\n\}\s*$",
            rpc_source,
            flags=re.DOTALL | re.MULTILINE,
        )
        # Pick the *first* match -- that's the Linux branch implementation,
        # not the NOSUPPORT fallback at the bottom of the file.
        body_match = re.search(
            r"alp_status_t\s+alp_rpc_call\s*\(",
            rpc_source,
        )
        assert body_match is not None
        # Slice from there to the end of file's Linux block (#else).
        else_match = re.search(r"#else\s+/\*\s*!__linux__", rpc_source)
        assert else_match is not None
        snippet = rpc_source[body_match.start(): else_match.start()]
        assert "pthread_mutex_lock(&ch->tx_mutex)" in snippet, \
            "alp_rpc_call must lock tx_mutex to serialise concurrent calls"
        assert "pthread_mutex_unlock(&ch->tx_mutex)" in snippet, \
            "alp_rpc_call must unlock tx_mutex on every return path"

    def test_close_wakes_pending_callers(self, rpc_source: str) -> None:
        """alp_rpc_close must wake any pending alp_rpc_call with
        ALP_ERR_NOT_READY so close-during-call doesn't leak the
        caller until its timeout expires."""
        m = re.search(
            r"void\s+alp_rpc_close\s*\([^)]*\)\s*\{(.*?)\n\}\s*\n",
            rpc_source,
            flags=re.DOTALL,
        )
        assert m is not None, "alp_rpc_close body not found"
        body = m.group(1)
        assert "call_pending" in body, \
            "alp_rpc_close must inspect call_pending"
        assert "ALP_ERR_NOT_READY" in body, \
            "alp_rpc_close must set the pending caller's result to NOT_READY"
        assert "pthread_cond_broadcast" in body or \
               "pthread_cond_signal" in body, \
            "alp_rpc_close must wake the pending caller's cond wait"

    def test_buffer_too_small_returns_nomem(self, rpc_source: str) -> None:
        """When the peer's response exceeds the caller's resp buffer
        capacity we must copy what fits AND return ALP_ERR_NOMEM
        (matching the alp/rpc.h documentation)."""
        # The RX worker is where the decision is made.
        # Look for the routing block that fires when call_pending is true.
        m = re.search(
            r"if\s*\(\s*ch->call_pending.*?ch->call_pending\s*=\s*false",
            rpc_source,
            flags=re.DOTALL,
        )
        assert m is not None, \
            "RX worker's response-routing block not found"
        block = m.group(0)
        assert "ALP_ERR_NOMEM" in block, \
            "RX worker must surface ALP_ERR_NOMEM when payload > resp_cap"
        assert "memcpy" in block, \
            "RX worker must memcpy the response into the caller's buffer"

    def test_correlation_strategy_matches_zephyr(
        self, rpc_source: str, zephyr_source: str
    ) -> None:
        """The Zephyr backend (src/backends/rpc/zephyr_drv.c) uses
        per-channel serialisation with a single call slot keyed on
        the method name.  The Linux backend MUST follow the same
        model so the two sides interop byte-for-byte.

        We check that neither backend prepends a 32-bit correlation
        ID to the frame.  Both rely on the method-name echo from the
        peer."""
        # Neither side should emit a correlation-ID symbol.
        for src, name in [
            (rpc_source, "rpc_yocto.c"),
            (zephyr_source, "src/backends/rpc/zephyr_drv.c"),
        ]:
            assert "corr_id" not in src and "correlation_id" not in src, \
                f"{name} appears to carry a correlation-ID field -- " \
                "Wave 5A picked the serialised per-channel model"

        # Both sides have a `call_pending` flag flipped from RX.
        assert "call_pending" in rpc_source
        assert "call_pending" in zephyr_source


# ---------------------------------------------------------------------
# Live tests (skipped on this host)
# ---------------------------------------------------------------------


def _have_openamp_user() -> bool:
    """Return True when libmetal + open-amp pkg-config metadata is
    available on the host.  Used to decide whether to build the
    .so and run the live tests."""
    if platform.system() != "Linux":
        return False
    pkg_config = shutil.which("pkg-config")
    if pkg_config is None:
        return False
    rc = os.system(f"{pkg_config} --exists open-amp libmetal >/dev/null 2>&1")
    return rc == 0


@pytest.mark.skipif(
    not _have_openamp_user(),
    reason=(
        "live alp_rpc_call tests require libmetal + open-amp user-space "
        "libraries (install libopen-amp-dev + libmetal-dev on Debian/Ubuntu, "
        "or the meta-openamp recipes on a Yocto build host).  The pre-built "
        ".so cannot be linked on this host."
    ),
)
class TestRpcYoctoLive:
    """End-to-end tests against the real rpc_yocto.c implementation.

    The harness builds librpc_yocto.so via CMake in ``setUp`` and
    substitutes the chardev path with a UNIX socketpair so the test
    process plays both sides of the RPMsg link.  Even on Linux the
    kernel's rpmsg subsystem is unavailable without remoteproc-backed
    hardware, so we mock the chardev at the file-descriptor level.

    These tests are intentionally light on assertions while the live
    harness matures.  They double as documentation of the contract
    the C side of Wave 5A must honour.
    """

    @pytest.fixture
    def mock_chardev(self, tmp_path):
        """Yield (request_fd_for_test_side, response_fd_for_test_side)
        plus a path the SUT opens via ALP_RPMSG_EPT_DEV.  Real
        implementations would intercept the open() syscall via
        LD_PRELOAD; we leave that to follow-up work."""
        pytest.skip(
            "live harness needs LD_PRELOAD-based open()/ioctl() shim "
            "for /dev/rpmsg_ctrl0 + /dev/rpmsg0; see follow-up issue"
        )

    def test_call_happy_path(self, mock_chardev) -> None:
        """alp_rpc_call returns ALP_OK + the peer's response payload
        when the peer replies with a frame whose method name matches
        the request."""

    def test_call_timeout(self, mock_chardev) -> None:
        """alp_rpc_call returns ALP_ERR_TIMEOUT when the peer never
        replies within timeout_ms."""

    def test_call_buffer_too_small(self, mock_chardev) -> None:
        """alp_rpc_call returns ALP_ERR_NOMEM + writes the truncated
        prefix + sets *resp_len to the actual response length when
        the response payload exceeds the caller's buffer capacity."""

    def test_call_close_during_pending(self, mock_chardev) -> None:
        """alp_rpc_call returns ALP_ERR_NOT_READY when alp_rpc_close
        fires from another thread while the call is blocked in
        pthread_cond_timedwait."""

    def test_call_concurrent_serialised(self, mock_chardev) -> None:
        """Per the public-API note, concurrent calls on the same
        channel are serialised: the second caller blocks until the
        first call returns or times out."""
