# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for the Yocto/Linux RPC backend
(``src/backends/rpc/yocto_drv.c``).

History: before the registry migration (#33) the public
``alp_rpc_*`` surface lived directly in ``src/yocto/rpc_yocto.c``.
That file was split: the public class dispatcher now owns the
``alp_rpc_open / close / subscribe / unsubscribe / send / call``
surface in ``src/rpc_dispatch.c``, and the Linux RPMsg
implementation was re-homed as a *backend* in
``src/backends/rpc/yocto_drv.c`` that exposes only the static
``y_*`` ops bound through an ``alp_rpc_ops_t`` vtable
(``_ops``) and registered via ``ALP_BACKEND_REGISTER``.

Two test classes:

* ``TestRpcYoctoStructure`` -- source-level structural tests that
  parse the yocto backend and assert the ops vtable, the framing
  helpers, the synchronous-call call-slot fields and the
  concurrency primitives are all present, and that the active
  (OpenAMP-userland) branch is a REAL implementation rather than
  the NOSUPPORT fallback.  ``TestRpcPublicSurface`` separately
  pins the moved-away public ``alp_rpc_*`` API to its new home in
  the dispatcher.  Both run on every host (Linux, macOS, Windows).

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
# Post-migration (#33) layout: the Linux RPMsg implementation is a
# backend (static y_* ops behind a vtable); the public alp_rpc_*
# surface lives in the shared class dispatcher.
RPC_SRC = REPO / "src" / "backends" / "rpc" / "yocto_drv.c"
RPC_DISPATCH = REPO / "src" / "rpc_dispatch.c"
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
def dispatch_source() -> str:
    assert RPC_DISPATCH.exists(), f"missing source: {RPC_DISPATCH}"
    return RPC_DISPATCH.read_text(encoding="utf-8")


@pytest.fixture(scope="module")
def zephyr_source() -> str:
    assert ZEPHYR_SRC.exists(), f"missing source: {ZEPHYR_SRC}"
    return ZEPHYR_SRC.read_text(encoding="utf-8")


@pytest.fixture(scope="module")
def header_source() -> str:
    assert RPC_HDR.exists(), f"missing header: {RPC_HDR}"
    return RPC_HDR.read_text(encoding="utf-8")


class TestRpcPublicSurface:
    """The public ``alp_rpc_*`` API moved OUT of the yocto source
    into the shared class dispatcher (``src/rpc_dispatch.c``) in the
    registry migration (#33).  Pin each public entry point to its
    new home so a future re-home or accidental deletion is caught."""

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
    def test_public_api_defined_in_dispatcher(
        self, dispatch_source: str, fn: str
    ) -> None:
        # Look for the function definition (name + opening paren).
        pattern = rf"\b{re.escape(fn)}\s*\("
        assert re.search(pattern, dispatch_source) is not None, (
            f"{fn} not found as a function definition in src/rpc_dispatch.c; "
            "the public alp_rpc_* surface lives in the class dispatcher "
            "post-#33, not in the yocto backend"
        )

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
    def test_public_api_not_redefined_in_backend(
        self, rpc_source: str, fn: str
    ) -> None:
        """The yocto backend must NOT re-define the public surface --
        it only provides the static ``y_*`` ops.  A stray public
        definition here would clash with the dispatcher at link time."""
        pattern = rf"\b{re.escape(fn)}\s*\("
        assert re.search(pattern, rpc_source) is None, (
            f"{fn} unexpectedly (re)defined in the yocto backend; the "
            "public surface belongs to src/rpc_dispatch.c after #33"
        )

    def test_dispatcher_delegates_to_backend_ops(
        self, dispatch_source: str
    ) -> None:
        """The dispatcher is a thin shim: every public call walks the
        selected backend's ``ops`` vtable rather than implementing the
        transport itself."""
        assert "alp_backend_select" in dispatch_source, (
            "dispatcher should select a backend via the registry"
        )
        for op in ["open", "subscribe", "unsubscribe", "send", "call", "close"]:
            assert re.search(rf"ops->{op}\b", dispatch_source) is not None, (
                f"dispatcher must delegate to ops->{op}"
            )


class TestRpcYoctoStructure:
    """Parse ``src/backends/rpc/yocto_drv.c`` and assert the
    synchronous-call backend has all the moving parts the design +
    the public header contract require.  These tests run on every
    host so a CI runner without libmetal still verifies the
    implementation didn't silently regress to the NOSUPPORT stub."""

    # ----- Backend ops surface --------------------------------------

    @pytest.mark.parametrize(
        "op",
        [
            "y_open",
            "y_close",
            "y_subscribe",
            "y_unsubscribe",
            "y_send",
            "y_call",
        ],
    )
    def test_backend_ops_defined(self, rpc_source: str, op: str) -> None:
        """Every alp_rpc_ops_t slot is backed by a static y_* op."""
        pattern = rf"\bstatic\s+(?:alp_status_t|void)\s+{re.escape(op)}\s*\("
        assert re.search(pattern, rpc_source) is not None, (
            f"{op} not found as a static op definition in yocto_drv.c"
        )

    @pytest.mark.parametrize(
        ("slot", "op"),
        [
            (".open", "y_open"),
            (".subscribe", "y_subscribe"),
            (".unsubscribe", "y_unsubscribe"),
            (".send", "y_send"),
            (".call", "y_call"),
            (".close", "y_close"),
        ],
    )
    def test_ops_table_wires_every_slot(
        self, rpc_source: str, slot: str, op: str
    ) -> None:
        """The static ``_ops`` table must bind every vtable slot to its
        y_* op.  We match the ``.slot = y_op`` initialiser directly."""
        pattern = rf"{re.escape(slot)}\s*=\s*{re.escape(op)}\b"
        assert re.search(pattern, rpc_source) is not None, (
            f"_ops table does not wire {slot} -> {op}"
        )

    def test_ops_table_present(self, rpc_source: str) -> None:
        assert re.search(
            r"static\s+const\s+alp_rpc_ops_t\s+_ops\s*=", rpc_source
        ) is not None, "static const alp_rpc_ops_t _ops table missing"

    def test_backend_registered(self, rpc_source: str) -> None:
        """The backend self-registers into the rpc class registry with
        the linux vendor + the _ops vtable."""
        m = re.search(
            r"ALP_BACKEND_REGISTER\s*\(\s*rpc\s*,\s*yocto_drv\s*,(.*?)\)\s*;",
            rpc_source,
            flags=re.DOTALL,
        )
        assert m is not None, "ALP_BACKEND_REGISTER(rpc, yocto_drv, ...) missing"
        block = m.group(1)
        assert re.search(r"\.vendor\s*=\s*\"linux\"", block), (
            "yocto backend must register with vendor \"linux\""
        )
        assert re.search(r"\.ops\s*=\s*&_ops", block), (
            "yocto backend registration must point .ops at &_ops"
        )

    def test_y_call_no_longer_nosupport(self, rpc_source: str) -> None:
        """The active (OpenAMP-userland) build of y_call must be a real
        implementation, NOT the NOSUPPORT fallback.

        The fallback NOSUPPORT path lives under the
        ``#else /* !ALP_SDK_HAVE_OPENAMP_USERLAND`` boundary; we slice
        the source at that boundary before checking so only the real
        branch is inspected."""
        m = re.search(
            r"#else\s+/\*\s*!ALP_SDK_HAVE_OPENAMP_USERLAND", rpc_source
        )
        assert m is not None, (
            "expected #else !ALP_SDK_HAVE_OPENAMP_USERLAND fallback block "
            "in yocto_drv.c"
        )
        real_branch = rpc_source[: m.start()]

        # Locate y_call's body inside the real branch.
        call_match = re.search(
            r"static\s+alp_status_t\s+y_call\s*\(.*?\)\s*\{",
            real_branch,
            flags=re.DOTALL,
        )
        assert call_match is not None, (
            "y_call not found in the OpenAMP-userland branch of yocto_drv.c"
        )

        # Walk braces to find the matching close.
        depth = 0
        i = call_match.end() - 1  # the '{' we just matched
        body_end = None
        while i < len(real_branch):
            c = real_branch[i]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    body_end = i
                    break
            i += 1
        assert body_end is not None, "could not balance braces"
        body = real_branch[call_match.end():body_end]

        # The real implementation must NOT short-circuit to NOSUPPORT.
        bad = re.findall(r"return\s+ALP_ERR_NOSUPPORT", body)
        assert not bad, (
            "y_call's OpenAMP-userland branch still returns "
            "ALP_ERR_NOSUPPORT; the re-home should have preserved the real "
            "implementation, not the stub"
        )

    def test_nosupport_fallback_present(self, rpc_source: str) -> None:
        """The non-OpenAMP host build still compiles to a clean
        NOSUPPORT op set so the TU links and the dispatcher falls
        through to the SW fallback."""
        m = re.search(
            r"#else\s+/\*\s*!ALP_SDK_HAVE_OPENAMP_USERLAND", rpc_source
        )
        assert m is not None
        fallback = rpc_source[m.start():]
        assert "ALP_ERR_NOSUPPORT" in fallback, (
            "the !ALP_SDK_HAVE_OPENAMP_USERLAND fallback must return "
            "ALP_ERR_NOSUPPORT from its ops"
        )

    # ----- File-header documentation -------------------------------

    def test_header_documents_correlation_strategy(
        self, rpc_source: str
    ) -> None:
        """The post-re-home file header should describe the actual
        implementation strategy (the call/response correlation model +
        the per-channel call-slot sync primitives) rather than a
        TODO."""
        head_match = re.search(r'#include\s+"alp/rpc\.h"', rpc_source)
        assert head_match is not None
        header_block = rpc_source[: head_match.start()]

        assert "TODO Wave 5" not in header_block
        assert "correlation strategy" in header_block.lower(), (
            "file header should explain the call/response correlation strategy"
        )
        # The correlation model is the method-name echo (no corr ID).
        assert "method name" in header_block.lower(), (
            "file header should describe the method-name-echo correlation model"
        )

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
        assert re.search(rf"\b{helper}\s*\(", rpc_source) is not None, (
            f"framing helper '{helper}' missing from yocto_drv.c"
        )

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
        exist in the backend's per-channel struct."""
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
        assert primitive in rpc_source, (
            f"expected '{primitive}' in yocto_drv.c"
        )

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
        must appear somewhere in yocto_drv.c."""
        assert code in rpc_source

    def test_call_serialises_via_tx_mutex(self, rpc_source: str) -> None:
        """The header docs (`@note Concurrent calls on the same
        channel from multiple threads are serialised by the SDK`)
        require y_call to take tx_mutex end-to-end."""
        # Slice the real (OpenAMP-userland) branch: from the first y_call
        # definition to the #else fallback boundary.
        body_match = re.search(
            r"static\s+alp_status_t\s+y_call\s*\(",
            rpc_source,
        )
        assert body_match is not None
        else_match = re.search(
            r"#else\s+/\*\s*!ALP_SDK_HAVE_OPENAMP_USERLAND", rpc_source
        )
        assert else_match is not None
        snippet = rpc_source[body_match.start(): else_match.start()]
        assert "pthread_mutex_lock(&ch->tx_mutex)" in snippet, (
            "y_call must lock tx_mutex to serialise concurrent calls"
        )
        assert "pthread_mutex_unlock(&ch->tx_mutex)" in snippet, (
            "y_call must unlock tx_mutex on every return path"
        )

    def test_close_wakes_pending_callers(self, rpc_source: str) -> None:
        """y_close must wake any pending y_call with ALP_ERR_NOT_READY
        so close-during-call doesn't leak the caller until its timeout
        expires."""
        m = re.search(
            r"static\s+void\s+y_close\s*\([^)]*\)\s*\{(.*?)\n\}\s*\n",
            rpc_source,
            flags=re.DOTALL,
        )
        assert m is not None, "y_close body not found"
        body = m.group(1)
        assert "call_pending" in body, (
            "y_close must inspect call_pending"
        )
        assert "ALP_ERR_NOT_READY" in body, (
            "y_close must set the pending caller's result to NOT_READY"
        )
        assert (
            "pthread_cond_broadcast" in body or "pthread_cond_signal" in body
        ), "y_close must wake the pending caller's cond wait"

    def test_buffer_too_small_returns_nomem(self, rpc_source: str) -> None:
        """When the peer's response exceeds the caller's resp buffer
        capacity we must copy what fits AND surface ALP_ERR_NOMEM
        (matching the alp/rpc.h documentation).  The decision is made
        in the RX worker's response-routing block."""
        m = re.search(
            r"if\s*\(\s*ch->call_pending.*?ch->call_pending\s*=\s*false",
            rpc_source,
            flags=re.DOTALL,
        )
        assert m is not None, "RX worker's response-routing block not found"
        block = m.group(0)
        assert "ALP_ERR_NOMEM" in block, (
            "RX worker must surface ALP_ERR_NOMEM when payload > resp_cap"
        )
        assert "memcpy" in block, (
            "RX worker must memcpy the response into the caller's buffer"
        )

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
        for src, name in [
            (rpc_source, "src/backends/rpc/yocto_drv.c"),
            (zephyr_source, "src/backends/rpc/zephyr_drv.c"),
        ]:
            assert "corr_id" not in src and "correlation_id" not in src, (
                f"{name} appears to carry a correlation-ID field -- "
                "both backends use the serialised per-channel model"
            )

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
    """End-to-end tests against the real yocto_drv.c backend.

    The harness builds the rpc backend into a .so via CMake in
    ``setUp`` and substitutes the chardev path with a UNIX socketpair
    so the test process plays both sides of the RPMsg link.  Even on
    Linux the kernel's rpmsg subsystem is unavailable without
    remoteproc-backed hardware, so we mock the chardev at the
    file-descriptor level.

    These tests are intentionally light on assertions while the live
    harness matures.  They double as documentation of the contract
    the C side must honour.
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
