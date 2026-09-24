# SPDX-License-Identifier: Apache-2.0
"""
Build-time guard for src/common/alp_fast_memcpy_core.c (issue #2285).

Compiles the core word-copy loop with the Zephyr SDK's arm-zephyr-eabi-gcc
using the arch/opt flags captured from a real `west build` of an AEN803
M55_HE Zephyr app's compile_commands.json (-mcpu=cortex-m55 -mthumb
-mabi=aapcs -mfp16-format=ieee -mtp=soft -Os, picolibc specs, no
--sysroot needed since -specs=picolibc.specs is self-contained in the
SDK) and checks the resulting object:

  - no undefined `memcpy` reference, and no `bl`/`b` branch to a
    <memcpy> symbol in the disassembly (the self-recursion trap: GCC's
    loop-idiom pass rewriting the loop back into a call to itself) --
    catches a future edit that drops or weakens the
    "no-tree-loop-distribute-patterns" guard before it ever reaches the
    bench and hangs a board before the network comes up (bench run 227).
    These two checks compile against _RECURSION_CHECK_FLAGS (-O2,
    WITHOUT -ffreestanding), not the real firmware's own -Os/-ffreestanding
    _COMPILE_FLAGS below: -ffreestanding suppresses GCC's
    -ftree-loop-distribute-patterns memcpy-pattern rewrite outright, and
    confirmed against GCC 14.3, this pass does not rewrite this exact
    loop shape at plain -Os either -- compiling these two checks at
    -Os/-ffreestanding would make them pass vacuously forever, guard
    present or not. Confirmed by hand against a scratch copy of
    src/common/alp_fast_memcpy_core.c with the
    no-tree-loop-distribute-patterns attribute removed: at
    _RECURSION_CHECK_FLAGS the scratch copy DOES recurse into `bl
    memcpy` (these two tests fail), and the real file (attribute
    present) stays clean (these two tests pass).
  - no MVE/FP register instruction: no v-prefixed mnemonic (vldrb,
    vstrb, vldrh, vstrh, vldrw, vstrw, vldrd, vstrd, vldm, vstm, vmov,
    ...) and no ldc/stc coprocessor-form load/store -- this SDK's
    objdump prints Cortex-M55 MVE Q-register load/store-multiple as the
    raw coprocessor encoding (`ldc 15, cr7, [r4], #16`), not as
    `vldrw.u32 q0, [r4], #16`; a regex that only looks for
    vldrw/vstrw/vldr/vstr misses this form completely. Catches a future
    edit (e.g. a re-added optimize("O2")) that lets GCC auto-vectorize
    the loop, putting memcpy() on FPU/MVE state everywhere it's called
    from, including an ISR built with FPU_SHARING=n.
  - at least one plain word load/store (ldr/str, no q-register operand)
    is present, so the test can't pass by the compiler having thrown the
    whole function away.

WHY -mfloat-abi=hard -mfpu=auto (not just any one current example's own
flags): CONFIG_ALP_SDK_FAST_MEMCPY's `depends on` list
(SOC_FAMILY_ENSEMBLE, SIZE_OPTIMIZATIONS, picolibc, GCC) does not
exclude CONFIG_FPU=y -- this file is shared code, compiled for *any*
AEN app that satisfies those four conditions. Not every current example
is soft-float either: examples/aen/aen-evk-demo already builds hard
float (-mfpu=fpv5-sp-d16 -mfloat-abi=hard), so this guard can't lean on
"soft-float is the only real case" -- but that concrete flag pair is
scalar-only VFP, not MVE-capable, so compiling this guard against it
alone would still make its MVE assertion vacuously true. -mfpu=auto
lets GCC pick this SoC's full MVE-capable FPU variant instead, which is
why hard-float + auto FPU (not aen-evk-demo's own -mfpu=fpv5-sp-d16) is
the worst case within this file's declared Kconfig support envelope,
and the only envelope in which the MVE assertion is a live, testable
invariant.
Confirmed locally: compiling the pre-fix code (the reverted
optimize("O2", "no-tree-loop-distribute-patterns") attribute) at
-Os/hard-float/auto-FPU produces `ldc 15, cr7, [r4], #16` /
`stc 15, cr7, [r4], #16` -- exactly the form the old regex missed --
while the current code stays clean under the same flags.

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

# The real AEN803 M55_HE arch/opt flags (captured from a real `west
# build` compile_commands.json), plus -mfloat-abi=hard -mfpu=auto -- see
# this file's header for why the FPU pair is the worst case within this
# option's declared Kconfig support envelope, not just aen-evk-demo's
# own hard-float flags.
_COMPILE_FLAGS = [
    "-std=c17",
    "-mcpu=cortex-m55",
    "-mthumb",
    "-mabi=aapcs",
    "-mfp16-format=ieee",
    "-mtp=soft",
    "-mfloat-abi=hard",
    "-mfpu=auto",
    "-Os",
    "-fno-strict-aliasing",
    "-fno-common",
    "-fno-pic",
    "-fno-pie",
    "-fno-asynchronous-unwind-tables",
    "-ftls-model=local-exec",
    "-fno-reorder-functions",
    "-fno-defer-pop",
    "-ffunction-sections",
    "-fdata-sections",
    "-specs=picolibc.specs",
    "-ffreestanding",
]

# For the two self-recursion checks only: -O2 instead of -Os, and no
# -ffreestanding. -ffreestanding suppresses GCC's
# -ftree-loop-distribute-patterns memcpy-pattern rewrite outright, and
# GCC 14.3 doesn't rewrite this exact loop shape at plain -Os either --
# compiling the recursion checks at the real firmware's own
# _COMPILE_FLAGS would make them pass vacuously forever, guard present
# or not. See this file's header for the by-hand confirmation (scratch
# copy, attribute removed) that this flag pair actually reproduces the
# rewrite.
_RECURSION_CHECK_FLAGS = [
    flag for flag in _COMPILE_FLAGS if flag not in ("-Os", "-ffreestanding")
] + ["-O2"]

# MVE/FP instruction: any v-prefixed mnemonic (vldrb/vstrb/vldrh/vstrh/
# vldrw/vstrw/vldrd/vstrd/vldm/vstm/vmov/...) or the ldc/stc
# coprocessor-encoding form this SDK's objdump uses for MVE Q-register
# load/store-multiple. Anchored to the mnemonic column (after the
# "offset:\thex bytes\t" prefix) so it can never match inside the hex
# opcode-byte column or an operand.
_MVE_FP_INSN_RE = re.compile(
    r"^\s*[0-9a-f]+:\t[0-9a-fA-F ]+\t(v[a-z0-9.]*|ldc[0-9l]*|stc[0-9l]*)\b",
    re.IGNORECASE | re.MULTILINE,
)

# A branch (bl, or a tail-call b/b.w/b.n) to a symbol objdump has
# annotated as exactly "<memcpy>" -- the self-recursion trap. Doesn't
# match "<alp_fast_memcpy_core+0x..>" (internal branches within this
# same function): that annotation never contains the literal substring
# "<memcpy>".
_MEMCPY_CALL_RE = re.compile(r"\bbl?(?:\.\w+)?\s+\S*<memcpy>", re.IGNORECASE)

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


def _compile_core(tmp_path_factory, tag: str, flags: list[str]) -> Path:
    gcc = _find_arm_zephyr_eabi_gcc()
    if gcc is None:
        pytest.skip("arm-zephyr-eabi-gcc (Zephyr SDK) not found on this host")

    out_dir = tmp_path_factory.mktemp(tag)
    obj_path = out_dir / "alp_fast_memcpy_core.o"

    result = subprocess.run(
        [
            gcc,
            "-c",
            *flags,
            "-I",
            str(CORE_C.parent),
            str(CORE_C),
            "-o",
            str(obj_path),
        ],
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    assert result.returncode == 0, (
        f"arm-zephyr-eabi-gcc failed to compile {CORE_C}:\n"
        f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    )
    return obj_path


@pytest.fixture(scope="module")
def compiled_object(tmp_path_factory) -> Path:
    return _compile_core(tmp_path_factory, "fast_memcpy_core_build_guard", _COMPILE_FLAGS)


@pytest.fixture(scope="module")
def compiled_object_recursion_check(tmp_path_factory) -> Path:
    """-O2, no -ffreestanding -- see _RECURSION_CHECK_FLAGS: the only flag
    pair (of the two compiled in this file) in which GCC's
    -ftree-loop-distribute-patterns memcpy-pattern rewrite is live against
    this exact loop shape under GCC 14.3."""
    return _compile_core(
        tmp_path_factory, "fast_memcpy_core_recursion_check", _RECURSION_CHECK_FLAGS
    )


def test_no_undefined_memcpy(compiled_object_recursion_check):
    """The self-recursion trap: a rewritten loop calls memcpy(), which the
    linker would need to resolve from somewhere -- catch that as an
    undefined `memcpy` symbol in the object, before it ever reaches a
    linked image. Compiled at _RECURSION_CHECK_FLAGS, not the real
    firmware's own -Os/-ffreestanding flags -- see this file's header."""
    gcc = _find_arm_zephyr_eabi_gcc()
    nm = _find_tool(gcc, "nm")
    result = subprocess.run(
        [nm, "-u", str(compiled_object_recursion_check)],
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    assert result.returncode == 0, result.stderr
    undefined = [line for line in result.stdout.splitlines() if "memcpy" in line]
    assert not undefined, (
        f"alp_fast_memcpy_core.c pulled in an undefined memcpy reference "
        f"(the self-recursion trap -- see the file header): {undefined}"
    )


def test_no_memcpy_self_call(compiled_object_recursion_check):
    """Belt-and-suspenders on the same self-recursion trap: no `bl memcpy`
    or tail-call `b memcpy` in the disassembly, independent of whether
    nm reports it as an undefined symbol. Compiled at
    _RECURSION_CHECK_FLAGS -- see this file's header."""
    gcc = _find_arm_zephyr_eabi_gcc()
    objdump = _find_tool(gcc, "objdump")
    result = subprocess.run(
        [objdump, "-d", str(compiled_object_recursion_check)],
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    assert result.returncode == 0, result.stderr

    calls = _MEMCPY_CALL_RE.findall(result.stdout)
    assert not calls, (
        f"alp_fast_memcpy_core.o branches to memcpy ({calls}) -- the "
        "self-recursion trap (see this file's header): a rewritten loop "
        "calling memcpy() calls back into this same loop, forever."
    )


def test_no_mve_vector_instructions(compiled_object):
    gcc = _find_arm_zephyr_eabi_gcc()
    objdump = _find_tool(gcc, "objdump")
    result = subprocess.run(
        [objdump, "-d", str(compiled_object)],
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    assert result.returncode == 0, result.stderr
    disasm = result.stdout

    mve_hits = _MVE_FP_INSN_RE.findall(disasm)
    assert not mve_hits, (
        "alp_fast_memcpy_core.o contains MVE/FP instructions "
        f"({mve_hits}) -- memcpy() must not touch FPU/MVE state (ISR "
        "clobber risk under FPU_SHARING=n; NOCP UsageFault before CPACR "
        "enable). See this file's Kconfig / source header."
    )
    assert _WORD_LDR_STR_RE.search(disasm), (
        "alp_fast_memcpy_core.o has no plain word ldr/str at all -- "
        "the compiler may have optimized the function away; check the "
        "compile command."
    )
