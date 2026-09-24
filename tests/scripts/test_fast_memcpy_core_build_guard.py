# SPDX-License-Identifier: Apache-2.0
"""
Build-time guard for src/common/alp_fast_memcpy_core.c (issue #2285).

Compiles the core word-copy loop with the Zephyr SDK's arm-zephyr-eabi-gcc
at -Os -mcpu=cortex-m55 (the real AEN Ensemble target profile) and checks
the resulting object:

  - no undefined `memcpy` reference (the self-recursion trap: GCC's
    loop-idiom pass rewriting the loop back into a call to itself) --
    catches a future edit that drops or weakens the
    "no-tree-loop-distribute-patterns" guard before it ever reaches the
    bench and hangs a board before the network comes up (bench run 227).
  - no Arm MVE Q-register vector load/store (vldrw/vstrw/vldr/vstr with a
    q<N> operand) -- catches a future edit (e.g. a re-added
    optimize("O2")) that lets GCC auto-vectorize the loop, putting
    memcpy() on FPU/MVE state everywhere it's called from, including an
    ISR built with FPU_SHARING=n.
  - at least one plain word load/store (ldr/str, no q-register operand)
    is present, so the test can't pass by the compiler having thrown the
    whole function away.

Skips cleanly (does not fail) when the Zephyr SDK's arm-zephyr-eabi-gcc
can't be found -- this machine-specific toolchain isn't guaranteed to be
on every host running the pytest suite; the native_sim ztest under
tests/unit/fast_memcpy is the mandatory correctness gate for this file,
this is an additional code-quality check where the toolchain is available.
"""
import glob
import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
CORE_C = REPO_ROOT / "src" / "common" / "alp_fast_memcpy_core.c"

# Q-register operand of an MVE vector load/store, e.g. "q0", "q3!" post-inc.
_MVE_VECTOR_INSN_RE = re.compile(r"\b(vldrw|vstrw|vldr|vstr)\.\S*\s+q\d", re.IGNORECASE)
_WORD_LDR_STR_RE = re.compile(r"\b(ldr|str)\b(?!.*\bq\d)", re.IGNORECASE)


def _find_arm_zephyr_eabi_gcc() -> str | None:
    found = shutil.which("arm-zephyr-eabi-gcc")
    if found:
        return found

    # The SDK layout has varied across releases (some put the toolchain
    # directly under <sdk>/arm-zephyr-eabi/, others nest it under an
    # intermediate <sdk>/gnu/) -- search recursively rather than pin one
    # layout.
    candidates = []
    sdk_dir = os.environ.get("ZEPHYR_SDK_INSTALL_DIR")
    if sdk_dir:
        candidates += glob.glob(
            os.path.join(sdk_dir, "**", "arm-zephyr-eabi", "bin", "arm-zephyr-eabi-gcc"),
            recursive=True,
        )
    candidates += glob.glob(
        os.path.expanduser("~/zephyr-sdk-*/**/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc"),
        recursive=True,
    )
    candidates += glob.glob(
        "/opt/zephyr-sdk-*/**/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc", recursive=True
    )

    for candidate in candidates:
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return None


def _find_tool(gcc_path: str, tool_suffix: str) -> str:
    """Given .../arm-zephyr-eabi-gcc, resolve the sibling arm-zephyr-eabi-<tool_suffix>."""
    bin_dir = os.path.dirname(gcc_path)
    candidate = os.path.join(bin_dir, f"arm-zephyr-eabi-{tool_suffix}")
    if os.path.isfile(candidate):
        return candidate
    # Fall back to PATH.
    found = shutil.which(f"arm-zephyr-eabi-{tool_suffix}")
    if found:
        return found
    pytest.skip(f"arm-zephyr-eabi-{tool_suffix} not found next to {gcc_path}")


@pytest.fixture(scope="module")
def compiled_object(tmp_path_factory) -> Path:
    gcc = _find_arm_zephyr_eabi_gcc()
    if gcc is None:
        pytest.skip("arm-zephyr-eabi-gcc (Zephyr SDK) not found on this host")

    out_dir = tmp_path_factory.mktemp("fast_memcpy_core_build_guard")
    obj_path = out_dir / "alp_fast_memcpy_core.o"

    result = subprocess.run(
        [
            gcc,
            "-c",
            "-Os",
            "-mcpu=cortex-m55",
            "-mthumb",
            "-ffreestanding",
            "-I",
            str(CORE_C.parent),
            str(CORE_C),
            "-o",
            str(obj_path),
        ],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, (
        f"arm-zephyr-eabi-gcc failed to compile {CORE_C}:\n"
        f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    )
    return obj_path


def test_no_undefined_memcpy(compiled_object):
    """The self-recursion trap: a rewritten loop calls memcpy(), which the
    linker would need to resolve from somewhere -- catch that as an
    undefined `memcpy` symbol in the object, before it ever reaches a
    linked image."""
    gcc = _find_arm_zephyr_eabi_gcc()
    nm = _find_tool(gcc, "nm")
    result = subprocess.run([nm, "-u", str(compiled_object)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    undefined = [line for line in result.stdout.splitlines() if "memcpy" in line]
    assert not undefined, (
        f"alp_fast_memcpy_core.c pulled in an undefined memcpy reference "
        f"(the self-recursion trap -- see the file header): {undefined}"
    )


def test_no_mve_vector_instructions(compiled_object):
    gcc = _find_arm_zephyr_eabi_gcc()
    objdump = _find_tool(gcc, "objdump")
    result = subprocess.run(
        [objdump, "-d", str(compiled_object)], capture_output=True, text=True
    )
    assert result.returncode == 0, result.stderr
    disasm = result.stdout

    mve_hits = _MVE_VECTOR_INSN_RE.findall(disasm)
    assert not mve_hits, (
        "alp_fast_memcpy_core.o contains MVE Q-register vector load/store "
        f"instructions ({mve_hits}) -- memcpy() must not touch FPU/MVE "
        "state (ISR clobber risk under FPU_SHARING=n; NOCP UsageFault "
        "before CPACR enable). See this file's Kconfig / source header."
    )
    assert _WORD_LDR_STR_RE.search(disasm), (
        "alp_fast_memcpy_core.o has no plain word ldr/str at all -- "
        "the compiler may have optimized the function away; check the "
        "compile command."
    )
