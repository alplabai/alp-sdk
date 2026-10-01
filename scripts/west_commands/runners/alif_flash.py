# Copyright (c) 2026 Alp Lab AB
#
# SPDX-License-Identifier: Apache-2.0

'''west runner: flash an AEN801 (Alif Ensemble E8) image to MRAM via the
Alif Security Toolkit (SETOOLS), so plain ``west flash`` runs the same
proven SES -> MCUboot -> slot0 provisioning recipe used by hand on the
bench.

This runner is the in-tree equivalent of
``scripts/bench/aen/flash-run.sh`` (Flow A). The Secure Enclave (SES)
is the only agent that programs MRAM on this part, so flashing is not a
SWD operation: it stages a per-app *signed ATOC* config, builds the ATOC
with ``app-gen-toc``, then burns it over the SE-UART with
``app-write-mram`` (the SES auto-enters maintenance, writes, resets, and
boots). See ``docs/aen-bench-bringup.md`` (§ Flow A) and
``docs/aen-provisioning.md``.

The ATOC entry shape is auto-detected from the app binary's OWN reset
vector (bytes 4-8 of zephyr.bin) -- no flag needed:

- An ITCM-linked app (reset vector 0x58xxxxxx on the M55-HE / 0x50xxxxxx
  on the M55-HP) gets an embedded ("load", "boot") ATOC entry; the SE
  copies it into ITCM before booting.
- A slot0-XIP app (reset vector inside App MRAM) gets a standalone
  ("boot"-only) ATOC entry at its OWN core's disjoint slot0 window
  (#1069) -- M55-HE at mramAddress 0x80010000 (unchanged), M55-HP at
  0x802b0000 (moved off the old shared 0x80010000 window; see
  ``scripts/aen_atoc.py`` and ``metadata/e1m_modules/E1M-AEN801.yaml``
  ``memory_map:``). Which window the reset vector falls in is what
  distinguishes the core here, cross-checked against ``--device`` the
  same way the ITCM branch below cross-checks it.

Bench-verified 2026-07-19 (pre-#1069, on the M55-HE window, since both
cores shared it then): a single ``app-write-mram -c <uart> -p`` run over
the SE-UART burns BOTH shapes -- for the slot0-XIP case it burns the
standalone app blob to 0x80010000 AND the signed ATOC in one pass (two
``COMMAND_BURN_MRAM`` phases, both confirmed by read-back: byte-exact at
0x80010000, reset vector 0x80011F15, core PC 0x80016048 running from
slot0). There is therefore no separate J-Link-only requirement for
slot0 provisioning; the two-blob bench Flow D helper
(``scripts/bench/aen/flash-jlink-mramxip.sh``) remains available as an
alternative, not a prerequisite.

Pre-burn ATOC-replace guard (#2262)
------------------------------------
``app-write-mram -c <uart> -p`` REPLACES the device's entire ATOC with
exactly the entries staged above (``DEVICE`` + this build's own
``ALP-HE``/``ALP-HP``) -- it is not a merge. Any OTHER resident app
entry (the other M55 core's own entry, an A32 Linux boot chain
``BOOTLOAD``/``A32_APP``, ...) is silently delisted the instant the
write lands, with no error and no SES warning (``[SES] ATOC ok`` prints
either way). This is the exact loss #2025 recorded on an AEN EVK bench
unit (2026-09-07), reached through this ``west flash`` door instead of
the bench scripts' door that #2025 already closed.

Before burning, ``do_run`` now reads the resident ATOC back over the
SE-UART with the same two documented, non-destructive SETOOLS queries
the bench guard uses (``maintenance -opt getbanner`` then ``-opt
gettoc``), and refuses the burn -- before anything is written -- when a
resident entry outside this run's own section would be delisted, or
when that read could not be verified (missing ``maintenance``, a
malformed/absent SES banner, or a non-zero query exit). ``--replace-atoc``
is the explicit override, spelled identically to
``scripts/bench/aen/flash-run.sh``'s own Flow A flag -- it is NOT Flow
D's ``--atoc-unqueryable`` (#2027) and the two are never merged or
aliased. The parse/decision logic is the single shared implementation in
``scripts/aen_atoc.py`` (``compute_query_status`` /
``foreign_resident_entries`` / ``decide_atoc_guard``), ported from (and
parity-tested against) ``bench_atoc_replace_guard`` in
``scripts/bench/aen/bench-env.sh`` so the two never drift apart. Every
run leaves a transcript + a machine-readable verdict under
``<build_dir>/alif_flash/`` (see ``_run_atoc_guard`` below).

Multi-domain sysbuild refusal (#2274)
--------------------------------------
Zephyr's own sysbuild flash plumbing runs this runner's ``do_run`` ONCE PER
DOMAIN, not once per ``west flash`` invocation (facts below cited against
Zephyr v4.4.1, this repo's pinned revision -- ``west.yml``'s ``zephyr:``
entry): ``scripts/west_commands/flash.py`` hands ``do_run_common`` the
sysbuild's top-level ``domains.yaml`` (``share/sysbuild/cmake/
domains.cmake`` generates it at ``${CMAKE_BINARY_DIR}/domains.yaml``, the
top build dir), and ``run_common.py``'s ``get_domains_to_process``/
``do_run_common`` loop instantiates and calls the board's flash runner
once per resolved domain, each time with THAT domain's own per-domain
``build_dir`` -- exactly one level below the top build dir where
``domains.yaml`` lives (``share/sysbuild/cmake/modules/
sysbuild_extensions.cmake`` sets each domain's binary dir to
``${CMAKE_BINARY_DIR}/${ZBUILD_APPLICATION}``).

The #2262 ATOC-replace guard above identifies a resident entry by NAME
ONLY. On an MCUboot sysbuild, both the ``mcuboot`` domain (ITCM
``0x58000000``) and the app domain (HE slot0) map to the SAME ATOC section
name ``ALP-HE`` via ``_atoc_section_name``. On a SECOND ``do_run``
invocation, any ``ALP-HE`` entry a previous ``alif_flash`` run of the
OTHER domain wrote is already this run's own ``allowed`` set, so the guard
reports ``clear`` and burns a fresh single-entry ATOC -- silently
replacing whatever that earlier run wrote. The guard only ever detects a
FOREIGN name; it has no way to see a same-name overwrite between two
sysbuild domains. (On a factory-provisioned module the resident MCUboot
entry is instead named ``MCUBOOT-`` (entry name TBD/unverified, see
``zephyr/sysbuild/aen/README.md``'s provisioning section), not
``ALP-HE`` -- the #2262 guard already refuses that case as a foreign
entry; this gap is specifically
about two ``alif_flash`` runs of the SAME repo's own sysbuild both writing
``ALP-HE``, e.g. a from-scratch bring-up or a re-keyed module.)

``do_run`` therefore refuses, before ANY staging/gettoc/burn side effect,
whenever this build's OWN ``domains.yaml`` (found via
``_parse_sysbuild_domains_yaml``, independent of how many domains THIS
west invocation happens to process) declares more than one FLASHABLE
domain -- including a single ``--domain <x>`` invocation of such a build,
since flashing only one domain would still replace any ``ALP-HE`` entry a
previous run of the other domain wrote, the same way. It also refuses,
with a distinct message, when `build_dir` is not even listed among the
domains its own ``domains.yaml`` declares at all (a relocated or stale
build tree). A single-domain sysbuild, and a plain (non-sysbuild) build
with no ``domains.yaml`` at all, are unaffected. See
``changelog.d/2274.md`` and ``docs/aen-provisioning.md`` §0.5 Option B for
the supported J-Link path.

Why this lives in alp-sdk and not upstream Zephyr
-------------------------------------------------
Upstream Zephyr's ``runners`` package has no ``alif_flash`` runner, and
ADR-0017 says we ride *over* the vendor SDK rather than patch it. This
module is therefore surfaced as a Zephyr *module runner* via
``zephyr/module.yml``'s ``runners:`` list -- ``west flash`` imports it at
flash time from the alp-sdk module, with no edit to the pinned Zephyr
tree. It imports only ``runners.core`` (Zephyr's stable runner base) +
the Python stdlib.

Setup (one-off, per host)
-------------------------
- Obtain the Alif Security Toolkit (license-gated, NOT redistributed by
  alp-sdk) and point this runner at it with ``--setools-dir`` or the
  ``SETOOLS_DIR`` environment variable.
- ``app-gen-toc`` depends on the ``fdt`` Python package, which is not a
  Zephyr requirement: ``pip install fdt`` once. (This runner does not
  import ``fdt`` itself; the SETOOLS executables do.)
'''

import importlib.util
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import yaml

from runners.core import RunnerCaps, ZephyrBinaryRunner


def _load_aen_atoc():
    '''Import scripts/aen_atoc.py (the #1069 ATOC-assembly guard) by file
    path rather than package import -- this module runs as a Zephyr
    module runner (imported via zephyr/module.yml's runners: list at
    `west flash` time), a context with no guarantee that alp-sdk's
    `scripts/` dir is already on sys.path.'''
    path = Path(__file__).resolve().parents[2] / 'aen_atoc.py'
    spec = importlib.util.spec_from_file_location('aen_atoc', path)
    mod = importlib.util.module_from_spec(spec)
    # Register in sys.modules BEFORE exec_module: aen_atoc.py's
    # AtocGuardVerdict (#2262) is a `dataclass`, and aen_atoc.py itself has
    # `from __future__ import annotations` -- so every field annotation is
    # stored as a STRING, and `dataclasses` resolves those strings (its
    # `_is_type` check against `KW_ONLY`/`ClassVar`/`InitVar`, present
    # since `KW_ONLY` landed in 3.10) by looking `cls.__module__` back up
    # through `sys.modules`. This is `dataclasses`' own standing behaviour
    # on every Python this repo targets (>=3.10, see pyproject.toml), not
    # a version-specific quirk -- an unregistered module (the bare
    # `module_from_spec()`+`exec_module()` pair used here before #2262)
    # makes that lookup return `None` and crash with `AttributeError:
    # 'NoneType' object has no attribute '__dict__'` the moment a
    # dataclass appears in this file. Harmless for every symbol that
    # existed before #2262; required now.
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


_aen_atoc = _load_aen_atoc()

# ATOC app-entry shapes. Both burn identically over the SE-UART
# (app-write-mram -c <uart> -p); which one applies to a given build is
# auto-detected from its reset vector (see _select_app_shape below), not
# a user flag -- bench-verified 2026-07-19 (see module docstring).
#
# ITCM-load (embedded): the app is linked to run from the M55 ITCM
# global alias; the SE copies it there before booting.
#   loadAddress 0x58000000 (M55-HE) / 0x50000000 (M55-HP).
#       cite: scripts/bench/aen/flash-run.sh:46  ("loadAddress": "0x58000000")
#       cite: docs/aen-provisioning.md section 4 ("Build the ATOC + write
#             it") -- the example ATOC config's ("loadAddress": "0x58000000")
#   flags ["load", "boot"], signed:true.
#       cite: scripts/bench/aen/flash-run.sh:45-46
#
# MRAM slot0 XIP (standalone): an app LINKED INTO MRAM slot0 (e.g. one
# that overflows ITCM, such as a real NPU model). The ATOC only tells
# the SE to boot it where it already sits (mramAddress), not to load it.
#   mramAddress = MRAM base 0x80000000 + this core's slot0 offset -- MUST
#   be the full address; the bare offset gives SETOOLS "Invalid Global
#   Address" (bench-pinned). Since #1069 the offset is per-core (disjoint
#   windows -- see scripts/aen_atoc.SLOT0_WINDOWS): M55-HE keeps the
#   pre-#1069 0x80010000, M55-HP moved to 0x802b0000.
#       cite: scripts/bench/aen/flash-jlink-mramxip.sh:45  (APP_ADDR=0x80010000, HE)
#       cite: scripts/bench/aen/flash-jlink-mramxip.sh:67  ("mramAddress": "0x80010000", HE)
#   flags ["boot"] only (NOT load -- the SE boots it in place); the slot0
#   reset vector then lands in MCUboot/the app at VTOR slot0_base + 0x800.
_CPU_PROFILES = {
    'HE': ('M55_HE', '0x58000000'),
    'HP': ('M55_HP', '0x50000000'),
}
_DEFAULT_CPU_SUFFIX = 'HE'  # boards that omit --device were always HE-only

# Same fallback as bench-env.sh's `${SE_UART_BAUD:-57600}` -- keep the two
# baud defaults identical so a host set up for the bench scripts needs no
# extra flag here either (#2262).
_DEFAULT_SE_UART_BAUD = '57600'

# TBD (unverified, no gettoc capture of a pre-provisioned module):  the
# ATOC entry name this code ASSUMES Alp Lab's factory provisioning stages
# for the MCUboot bootloader on a pre-provisioned module (HIGH-2 review,
# #2262). It traces to exactly one sentence in
# `zephyr/sysbuild/aen/README.md`'s "SoM-maker provisioning model" --
# `cpu_id M55_HE`, `loadAddress 0x58000000`, and the SES banner then shows
# `| MCUBOOT- | M55-HE | ... | uLVB |` -- which itself documents a
# SETOOLS-shipped config (`build/config/app-mcuboot-only.json`) that
# alp-sdk does not ship. There is NO capture of a real pre-provisioned
# module's `gettoc` output anywhere in this repo to confirm the literal
# against; every "real" fixture in tests/scripts/test_atoc_guard_parity.py
# is an AEN EVK *dev* board, never a factory-provisioned one. The name is
# plausible (`docs/aen-provisioning.md`'s Option B example uses the same
# `BLINK-HE`-style convention), but if it is wrong this constant does NOT
# make the guard unsafe: an unrecognised factory name still resolves as an
# ordinary foreign entry and the guard still REFUSES -- it only loses the
# distinct, more specific message below and falls through to the generic
# one (see that message's own note on why it no longer defaults to
# steering the operator at `--replace-atoc`). Capture the real name during
# a Stage 2 bench session (see this PR's bench plan) and correct this
# literal then; until that capture exists, treat it as TBD, not confirmed.
_FACTORY_MCUBOOT_ATOC_NAME = 'MCUBOOT-'


def _cpu_suffix(device):
    '''Return 'HE' or 'HP' from the trailing _HE/_HP token of --device
    (board.cmake's board_runner_args(alif_flash --device=...), or the
    -i/--dev-id alias); defaults to HE when device is unset.'''
    if device:
        upper = device.upper()
        if upper.endswith('_HP'):
            return 'HP'
        if upper.endswith('_HE'):
            return 'HE'
    return _DEFAULT_CPU_SUFFIX


def _reset_vector(bin_path):
    '''Read the little-endian reset-vector word (the Cortex-M vector
    table's 2nd entry, offset 4, right after the initial SP) out of a
    raw zephyr.bin image -- the fact that says where this build is
    linked to run from.'''
    with open(bin_path, 'rb') as f:
        header = f.read(8)
    if len(header) < 8:
        raise RuntimeError(
            f"'{bin_path}' is only {len(header)} bytes -- too small to "
            'contain a Cortex-M vector table (need >= 8 bytes for the '
            'initial SP + reset vector).')
    return int.from_bytes(header[4:8], 'little')


def _select_app_shape(reset_vector, cpu_suffix):
    '''Map a build's reset vector to its ATOC app-entry shape. Both
    shapes burn identically over the SE-UART (see module docstring);
    this only selects the config, it never rejects a flash (except a
    core mismatch -- see the TCM/slot0-window branches below).'''
    if 0x80010000 <= reset_vector < _aen_atoc.MRAM_END:
        # App-MRAM slot0 XIP region. Since #1069 each core has its OWN
        # disjoint window (scripts/aen_atoc.SLOT0_WINDOWS) -- find which
        # window contains this build's reset vector, the same way the
        # ITCM branch below finds the core from the vector's top byte,
        # then cross-check --device agrees.
        # #1981 review: scripts/aen_atoc.SLOT0_WINDOWS['A32_0'] is (by
        # design, per that module's docstring) a window covering
        # essentially the whole low App MRAM span, INCLUDING a margin
        # above M55_HP's own window ceiling that belongs to neither real
        # M55 window. This runner is Zephyr `west flash`-only (see
        # _CPU_PROFILES/capabilities()) and never stages an A32 Linux
        # image, so A32_0 is excluded from this lookup entirely -- an
        # earlier version matched against it and reported a genuine M55
        # mis-link into that margin (e.g. 0x80560000) as "staged by a
        # different flash path", masking a real refusal-worthy error
        # behind a message that means "nothing to see here".
        vector_cpu_id = None
        for candidate_cpu_id, (win_base, win_size) in _aen_atoc.SLOT0_WINDOWS.items():
            if candidate_cpu_id not in _aen_atoc.M55_CPU_IDS:
                continue
            if win_base <= reset_vector < win_base + win_size:
                vector_cpu_id = candidate_cpu_id
                break
        if vector_cpu_id is None:
            windows_desc = ', '.join(
                f'{cid} 0x{b:08x}..0x{b + s:08x}'
                for cid, (b, s) in _aen_atoc.SLOT0_WINDOWS.items()
                if cid in _aen_atoc.M55_CPU_IDS)
            raise RuntimeError(
                f'reset vector 0x{reset_vector:08x} is in App MRAM but '
                f'outside every declared slot0 window ({windows_desc}) -- '
                'refusing to burn (see metadata/e1m_modules/'
                'E1M-AEN801.yaml memory_map:).')
        vector_suffix = 'HE' if vector_cpu_id == 'M55_HE' else 'HP'
        if vector_suffix != cpu_suffix:
            raise RuntimeError(
                f'reset vector 0x{reset_vector:08x} is linked into the '
                f'{vector_cpu_id} slot0 window but --device selected '
                f'M55-{cpu_suffix} -- refusing to burn a core-mismatched '
                'image (check --device against the actual link address).')
        cpu_id, _unused_load_address = _CPU_PROFILES[cpu_suffix]
        win_base, _win_size = _aen_atoc.SLOT0_WINDOWS[cpu_id]
        return {
            'cpu_id': cpu_id,
            'mramAddress': f'0x{win_base:08x}',
            'flags': ['boot'],
        }
    core_byte = reset_vector & 0xFF000000
    if core_byte in (0x58000000, 0x50000000):
        # ITCM: the vector's top byte IS the core (0x58 HE / 0x50 HP) --
        # trust it over --device for cpu_id/loadAddress, then cross-check
        # --device agrees (defense-in-depth against a wrong/absent
        # --device masking a core-mismatched image).
        vector_suffix = 'HE' if core_byte == 0x58000000 else 'HP'
        if vector_suffix != cpu_suffix:
            raise RuntimeError(
                f'reset vector 0x{reset_vector:08x} is linked for the '
                f'M55-{vector_suffix} ITCM alias but --device selected '
                f'M55-{cpu_suffix} -- refusing to burn a core-mismatched '
                'image (check --device against the actual link address).')
        cpu_id, load_address = _CPU_PROFILES[vector_suffix]
        return {
            'cpu_id': cpu_id,
            'loadAddress': load_address,
            'flags': ['load', 'boot'],
        }
    raise RuntimeError(
        f'unrecognised reset vector 0x{reset_vector:08x} -- expected a '
        f'slot0-XIP image in App MRAM (0x80010000..0x{_aen_atoc.MRAM_END:08x}) '
        'or an ITCM-load image (0x58xxxxxx M55-HE / 0x50xxxxxx M55-HP). '
        'Refusing to burn a mismatched image (see docs/aen-provisioning.md).')


def _atoc_section_name(app_shape):
    '''The ATOC entry name this build's write owns -- 'ALP-HE' or
    'ALP-HP', derived from app_shape['cpu_id'] ('M55_HE'/'M55_HP'). The
    ONE place this string is built: both _build_atoc_config (what gets
    staged) and the pre-burn guard's `allowed` set (#2262 -- what the
    guard permits this run to (re)write) call this, so the two can never
    drift apart into staging one section while guarding another.'''
    return f'ALP-{app_shape["cpu_id"].split("_")[1]}'


def _build_atoc_config(name, app_shape):
    '''Return the signed-ATOC JSON for a build. DEVICE keeps the
    factory device config (signed); the app entry is signed + shaped per
    app_shape (see _select_app_shape) -- an ITCM-load entry
    (loadAddress) or an MRAM slot0-XIP entry (mramAddress).'''
    addr_field = (
        f'"mramAddress": "{app_shape["mramAddress"]}"'
        if 'mramAddress' in app_shape
        else f'"loadAddress": "{app_shape["loadAddress"]}"')
    flags = ', '.join(f'"{flag}"' for flag in app_shape['flags'])
    app_entry = (
        f'                 "cpu_id": "{app_shape["cpu_id"]}", '
        f'{addr_field}, '
        f'"flags": [{flags}] }}')
    section = _atoc_section_name(app_shape)
    return (
        '{\n'
        '    "DEVICE":  { "disabled": false, '
        '"binary": "app-device-config.json", "version": "0.5.00", '
        '"signed": true },\n'
        f'    "{section}":  {{ "disabled": false, '
        f'"binary": "{name}.bin", "version": "1.0.0", "signed": true,\n'
        f'{app_entry}\n'
        '}\n')


# Multi-domain sysbuild refusal (#2274) -- see the module docstring section
# of the same name for the upstream Zephyr facts this relies on. Every
# `path:line` citation below is against Zephyr v4.4.1, this repo's pinned
# revision (west.yml's `zephyr:` entry) -- a stable citation, not a moving
# target, since alp-sdk pins one Zephyr base and bumps it deliberately.


def _sysbuild_domains_path(build_dir):
    '''The path this runner checks for a sysbuild ``domains.yaml``, given
    the PER-DOMAIN ``build_dir`` `west flash` hands `do_run` (`self.cfg.
    build_dir`).

    ``domains.yaml`` itself lives at the sysbuild's TOP build_dir, one
    level ABOVE the per-domain build_dir this runner receives:

    - ``share/sysbuild/cmake/domains.cmake:8`` sets
      ``build_dir: ${CMAKE_BINARY_DIR}`` (the top build dir) inside the
      generated YAML, and line 19 writes the file itself to
      ``${CMAKE_BINARY_DIR}/domains.yaml``.
    - ``share/sysbuild/cmake/modules/sysbuild_extensions.cmake:356,360``
      sets each domain's OWN binary dir to
      ``${CMAKE_BINARY_DIR}/${ZBUILD_APPLICATION}`` -- i.e. exactly one
      path component below the top build dir where ``domains.yaml`` sits.
    - ``scripts/west_commands/flash.py:32`` builds
      ``Path(build_dir) / 'domains.yaml'`` from that SAME top build_dir
      (``get_build_dir(args)``, not any one domain's own build_dir) before
      calling ``do_run_common``; ``scripts/west_commands/run_common.py:
      351-353`` (``for d in domains: ... do_run_common_image(..., d.
      build_dir, ...)``) is what then hands EACH domain's OWN
      ``d.build_dir`` to the board's runner as ``cfg.build_dir`` -- the
      value this function receives.

      Uses ``.absolute()``, NOT ``.resolve()``: this only needs an
      absolute path to compute ``.parent`` correctly (a relative
      `build_dir` from a west invocation run outside the build tree's own
      directory) -- it must NOT follow a symlink in `build_dir` itself,
      since that would locate ``domains.yaml`` relative to the symlink's
      TARGET rather than the tree the caller (and Zephyr's own
      `flash.py`) actually navigated. Symlink resolution belongs only to
      the listed-build-dir COMPARISON in
      `_refuse_if_multi_domain_sysbuild`, where both sides must be
      resolved the same way for equality to be meaningful -- not to
      locating the file to read in the first place.'''
    return Path(build_dir).absolute().parent / 'domains.yaml'


def _parse_sysbuild_domains_yaml(build_dir):
    '''Return ``{"all": [...], "flashable": [...]}`` -- both lists of
    ``{"name": ..., "build_dir": ...}`` dicts -- for this build's own
    ``domains.yaml``, or ``None`` if it is not a sysbuild build at all (no
    ``domains.yaml`` next to the top build dir `build_dir` is nested
    under). Pure and testable without a real west/Zephyr workspace.

    ``"all"`` is every entry under ``domains:``, used to check whether
    `build_dir` is even one of this sysbuild's own domains at all (see
    `_refuse_if_multi_domain_sysbuild`'s stale-tree check) -- a build-only
    image's build_dir is still a legitimate `cfg.build_dir` in principle
    (a `--domain <build-only-image>` invocation, however unusual), so it
    must not be misreported as "stale".

    ``"flashable"`` is what would actually be FLASHED, preferring
    ``flash_order:`` over the full ``domains:`` list when present and
    non-empty: ``share/sysbuild/cmake/domains.cmake:14-16`` builds
    ``flash_order:`` from ``IMAGES_FLASHING_ORDER`` filtered to exclude
    each image's ``BUILD_ONLY`` property, i.e. it is already the
    flashable-only subset -- the SAME list
    ``scripts/west_commands/run_common.py:213``'s
    ``domains.get_domains(args.domain, default_flash_order=True)`` reads
    for a plain multi-domain ``west flash`` with no ``--domain``. Falls
    back to ``"all"`` only when ``flash_order:`` is absent or empty, on
    the fail-closed side (a domains.yaml shape this parser does not
    recognise still counts every domain rather than none).

    Reads only the documented ``domains.yaml`` shape
    (``share/sysbuild/cmake/domains.cmake``): a mapping with ``default``,
    ``build_dir``, a ``domains:`` sequence of ``{name, build_dir}`` maps,
    and an optional ``flash_order:`` sequence of names. Deliberately does
    not import Zephyr's own ``Domains`` class
    (``scripts/pylib/build_helpers/domains.py``) -- that module is only
    importable from an active west workspace, and this runner's own module
    docstring explains why it avoids such a dependency (Zephyr module
    runner, imported at flash time with no guarantee of anything beyond
    ``runners.core`` on ``sys.path``).

    Fails CLOSED on anything unreadable, malformed, or degenerate: an
    unparseable ``domains.yaml``, a ``domains:``/``flash_order:`` entry of
    the wrong shape or type, or an EMPTY ``domains:`` list, must never be
    silently treated as "not a sysbuild" or "single domain, proceed" --
    each of those would defeat the very refusal this function exists to
    drive. Raises ``RuntimeError`` in every such case, matching this
    runner's existing error-raising idiom (see e.g. ``_reset_vector``,
    ``_select_app_shape``).'''
    domains_yaml = _sysbuild_domains_path(build_dir)
    if not domains_yaml.is_file():
        return None
    try:
        text = domains_yaml.read_text(encoding='utf-8')
    except OSError as exc:
        raise RuntimeError(
            f"could not read '{domains_yaml}' to check whether this is a "
            f'multi-domain sysbuild ({exc}) -- refusing rather than '
            'assuming a single-domain build (alp-sdk#2274).') from exc
    try:
        data = yaml.safe_load(text)
    except yaml.YAMLError as exc:
        raise RuntimeError(
            f"'{domains_yaml}' is not valid YAML ({exc}) -- cannot verify "
            'this is not a multi-domain sysbuild; refusing rather than '
            'assuming a single-domain build (alp-sdk#2274).') from exc
    if not isinstance(data, dict) or not isinstance(data.get('domains'), list):
        raise RuntimeError(
            f"'{domains_yaml}' does not match the expected domains.yaml "
            "shape (a mapping with a 'domains:' list) -- cannot verify "
            'this is not a multi-domain sysbuild; refusing rather than '
            'assuming a single-domain build (alp-sdk#2274).')
    if not data['domains']:
        raise RuntimeError(
            f"'{domains_yaml}' declares an empty domains: list -- cannot "
            'verify this is not a multi-domain sysbuild; refusing rather '
            'than assuming a single-domain build (alp-sdk#2274).')

    by_name = {}
    for entry in data['domains']:
        if (not isinstance(entry, dict) or 'name' not in entry
                or 'build_dir' not in entry):
            raise RuntimeError(
                f"'{domains_yaml}' has a domains: entry missing 'name' or "
                "'build_dir' -- cannot verify this is not a multi-domain "
                'sysbuild; refusing rather than assuming a single-domain '
                'build (alp-sdk#2274).')
        name, entry_build_dir = entry['name'], entry['build_dir']
        if not isinstance(name, str) or not isinstance(entry_build_dir, str):
            raise RuntimeError(
                f"'{domains_yaml}' has a domains: entry whose 'name' or "
                "'build_dir' is not a string -- cannot verify this is not "
                'a multi-domain sysbuild; refusing rather than assuming a '
                'single-domain build (alp-sdk#2274).')
        if name in by_name:
            raise RuntimeError(
                f"'{domains_yaml}' has a duplicate domains: entry named "
                f'{name!r} -- cannot verify this is not a multi-domain '
                'sysbuild; refusing rather than assuming a single-domain '
                'build (alp-sdk#2274).')
        by_name[name] = {'name': name, 'build_dir': entry_build_dir}
    all_domains = list(by_name.values())

    flash_order = data.get('flash_order')
    if isinstance(flash_order, list) and flash_order:
        flashable = []
        for name in flash_order:
            if not isinstance(name, str):
                raise RuntimeError(
                    f"'{domains_yaml}' has a flash_order: entry that is "
                    'not a string -- cannot verify this is not a '
                    'multi-domain sysbuild; refusing rather than assuming '
                    'a single-domain build (alp-sdk#2274).')
            if name not in by_name:
                raise RuntimeError(
                    f"'{domains_yaml}' flash_order: names {name!r}, which "
                    'is not declared under domains: -- cannot verify this '
                    'is not a multi-domain sysbuild; refusing rather than '
                    'assuming a single-domain build (alp-sdk#2274).')
            flashable.append(by_name[name])
    else:
        flashable = all_domains

    return {'all': all_domains, 'flashable': flashable}


def _refuse_if_multi_domain_sysbuild(build_dir):
    '''Refuse (RuntimeError) before ANY staging/gettoc/J-Link/burn side
    effect when `build_dir`'s own sysbuild declares more than one
    FLASHABLE domain (see `_parse_sysbuild_domains_yaml`'s ``"flashable"``)
    -- keyed on the BUILD's domains.yaml, not on how many domains this
    west invocation happens to process, so a single `--domain <x>`
    invocation of a 2-domain sysbuild refuses too (see module docstring,
    "Multi-domain sysbuild refusal (#2274)").

    Also refuses (with a DISTINCT message) when `build_dir` is not even
    LISTED among its own domains.yaml's domains at all -- a
    relocated/stale build tree whose domains.yaml no longer describes
    where this build actually lives. Comparing RESOLVED paths (`Path(
    build_dir).resolve()` against each `Path(entry['build_dir']).resolve()`)
    rather than raw strings means a relative vs. absolute spelling of the
    same directory still matches; `.resolve()` does not require the path
    to exist (`strict=False` is pathlib's default), so a not-yet-built
    domain build_dir is handled the same way.'''
    if not build_dir:
        return
    domains_yaml = _sysbuild_domains_path(build_dir)
    parsed = _parse_sysbuild_domains_yaml(build_dir)
    if parsed is None:
        return

    this_build_dir = Path(build_dir).resolve()
    listed = any(Path(e['build_dir']).resolve() == this_build_dir
                 for e in parsed['all'])
    if not listed:
        raise RuntimeError(
            f"'{domains_yaml}' does not list this build dir "
            f"'{this_build_dir}' -- stale sysbuild output? remove it or "
            'build elsewhere (alp-sdk#2274).')

    flashable = parsed['flashable']
    if len(flashable) <= 1:
        return

    domains_desc = ', '.join(e['name'] for e in flashable)
    raise RuntimeError(
        f"'{domains_yaml}' declares a multi-domain sysbuild "
        f'({domains_desc}) -- refusing to flash it through the '
        "alif_flash runner (alp-sdk#2274). Zephyr's own sysbuild flash "
        "plumbing runs this runner's do_run once PER DOMAIN, and the "
        '#2262 ATOC-replace guard identifies a resident entry by NAME '
        'ONLY: the mcuboot domain and the HE app domain both map to the '
        "SAME ATOC section name 'ALP-HE', so a second invocation would "
        'see any ALP-HE entry a previous alif_flash run of the other '
        'domain wrote as already-allowed and burn a fresh single-entry '
        'ATOC over it -- the guard reports clear, but that is silently '
        'replacing whatever the earlier run wrote. This holds even when '
        'flashing only ONE domain (e.g. --domain app): it would still '
        'replace any ALP-HE entry a previous alif_flash run of the other '
        'domain wrote, the same way. Use the supported path instead: a '
        'plain J-Link loadbin of your imgtool-signed image straight to '
        'slot0, no SETOOLS/ATOC/SE-UART at all (docs/aen-provisioning.md '
        'section 0.5, Option B). That J-Link path writes the APP ONLY -- '
        'it requires MCUboot already resident (a pre-provisioned module). '
        'On a bare, wiped, or re-keyed module, provision MCUboot with '
        "SETOOLS first (zephyr/sysbuild/aen/README.md's provisioning "
        'section) or the module will not boot (recoverable via SETOOLS '
        're-provisioning).')


# MEDIUM-6 review (#2262): a wedged SE-UART (the maintenance session
# never returns) must not hang `west flash` forever -- 120s comfortably
# covers a real `getbanner`/`gettoc` round trip (bench captures complete
# in well under 1s) while still failing well before an operator gives up
# and kills the process by hand, which would leave no transcript at all.
_MAINTENANCE_TIMEOUT_S = 120


def _run_maintenance(maintenance_path, se_uart, baud, opt):
    '''Run `maintenance -b <baud> -c <se_uart> -opt <opt>` from
    maintenance_path's own directory (mirrors bench-env.sh's `( cd
    "$SETOOLS_DIR" && ./maintenance ... )`), combining stdout+stderr into
    one transcript (mirrors its `2>&1`). Returns (output_text, returncode)
    and never raises on a non-zero exit -- a gettoc that fails after
    emitting partial table rows (a serial timeout mid-read) is a signal
    the #2262 guard must see, not an exception to swallow. A timeout
    (`_MAINTENANCE_TIMEOUT_S`) is the SAME fail-closed shape: whatever
    partial output the process produced before being killed, rc=1 --
    `compute_query_status` already treats any non-zero rc as
    `unverified`, never "ok" from partial text.

    NEW-1 review (#2262): deliberately does NOT pass `text=True` (nor
    `encoding=`/`errors=` to `subprocess.run`, which imply it). `text=True`
    turns on Python's universal-newline translation, which rewrites every
    BARE `\\r` in the child's stdout to `\\n` -- splitting one line into
    two. bash's own `maintenance`-reading code (bench-env.sh) does the
    OPPOSITE: `tr -d '\\r'` DELETES a bare `\\r`, keeping the rest of the
    line intact, and the banner check does not even do that (a `\\r`
    embedded mid-banner is just an ordinary character to `grep`, which
    only ever splits on `\\n`). Under `text=True`, a real capture like
    `"junk\\rSES A1 v1.110.0 ..."` (embedded `\\r`, no real `\\n`) was
    measured to SPLIT into two lines, the second of which parses as a
    valid SES banner on its OWN line -- accepting a banner bash's `grep`
    correctly rejects as one non-matching line. Capturing raw `bytes` and
    decoding by hand below leaves every `\\r` exactly where the SE-UART
    put it, so `parse_resident_atoc_table`'s own `.replace('\\r', '')`
    (this module's mirror of `tr -d '\\r'`) and `is_valid_ses_banner`'s
    lack of any CR handling both see the SAME bytes bash's `grep`/`tr` do.
    See tests/scripts/test_alif_flash_runner.py's
    `test_run_maintenance_*_bare_cr_*` tests and
    tests/scripts/test_atoc_guard_parity.py's `..._through_run_maintenance`
    parity cases for the exact shapes this closes.'''
    try:
        proc = subprocess.run(
            [str(maintenance_path), '-b', str(baud), '-c', se_uart,
             '-opt', opt],
            cwd=str(maintenance_path.parent), stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=_MAINTENANCE_TIMEOUT_S)
    except subprocess.TimeoutExpired as exc:
        # No `text=True` above, so `exc.stdout` (whatever the child wrote
        # before the kill) is always `bytes` here -- still guard against
        # `None` (nothing captured yet).
        partial = exc.stdout.decode('utf-8', errors='replace') if exc.stdout else ''
        return f'{partial}\nTIMEOUT after {_MAINTENANCE_TIMEOUT_S}s: {exc}\n', 1
    except OSError as exc:
        return f'{exc}\n', 1
    except ValueError as exc:
        # Review finding (#2262, second round): `subprocess.run` raises
        # `ValueError`, not `OSError`, for some argument-shape problems it
        # catches before ever exec'ing anything -- e.g. a NUL byte
        # embedded in an argument (a garbled/truncated `se_uart` string).
        # Treat it the same as any other failed-to-even-run case: rc=1,
        # never an uncaught exception out of the guard.
        return f'{exc}\n', 1
    return proc.stdout.decode('utf-8', errors='replace'), proc.returncode


def _format_atoc_transcript(se_uart, baud, maintenance_available,
                             banner_text, banner_rc, gettoc_text,
                             gettoc_rc):
    '''Render the #2262 pre-burn transcript kept at
    <build_dir>/alif_flash/atoc-before.txt -- always the record of what
    was resident (or why it could not be read) immediately before the
    burn step, whether or not the run went on to burn.'''
    if not maintenance_available:
        return (
            f"GUARD: SETOOLS 'maintenance' tool not found next to "
            f"app-write-mram -- cannot query the resident ATOC over "
            f"{se_uart}\n")
    return (
        f'$ maintenance -b {baud} -c {se_uart} -opt getbanner  '
        f'(exit {banner_rc})\n'
        f'{banner_text or ""}\n'
        f'$ maintenance -b {baud} -c {se_uart} -opt gettoc  '
        f'(exit {gettoc_rc})\n'
        f'{gettoc_text or ""}\n')


def _write_text_or_raise(path, content):
    '''`Path.write_text`, but a failure (an unwritable build_dir -- a
    read-only mount, a permissions mistake) is wrapped and re-raised as a
    RuntimeError NAMING THE PATH instead of propagating a bare OSError.

    Review finding (#2262, second round): this was the ONE abort path in
    `_run_atoc_guard` that did not produce a RuntimeError -- still
    fail-closed (the burn is never reached; whatever raises here happens
    strictly before `app-write-mram` runs), but silent about which of the
    guard's own writes (the transcript or the verdict) failed and why, and
    it necessarily writes NO atoc-guard.json when it is the verdict write
    itself that fails -- the one honest exception to this guard's own "a
    refused run always leaves a verdict" contract (documented in
    docs/aen-provisioning.md's "The ATOC-replace guard verdict contract"
    section): if the build directory cannot be written to at all, there is
    nowhere to put that verdict.'''
    try:
        # write-text-newline-exempt: scratch per-run alif_flash ATOC guard artifact
        path.write_text(content, encoding='utf-8')
    except OSError as exc:
        raise RuntimeError(
            f'could not write {path} ({exc}) -- refusing to burn without '
            'a place to record the pre-burn ATOC guard\'s own result.'
        ) from exc


class AlifFlashBinaryRunner(ZephyrBinaryRunner):
    '''Flash an AEN801 image to MRAM with the Alif SETOOLS (Flow A).'''

    def __init__(self, cfg, device, setools_dir=None, se_uart=None,
                 gen_toc='app-gen-toc', write_mram='app-write-mram',
                 se_uart_baud=None, replace_atoc=False):
        super().__init__(cfg)
        # --device first 5 chars must match the SETOOLS global-cfg.db
        # Part# ("AE822..."); the runner forwards it for the staged
        # config + diagnostics AND to pick the M55-HE/HP ATOC shape (see
        # _cpu_suffix). cite: board.cmake comment + :39.
        self.device = device
        self.setools_dir = setools_dir
        self.se_uart = se_uart
        self.gen_toc = gen_toc
        self.write_mram = write_mram
        # Pre-burn ATOC guard (#2262) -- see the module docstring section
        # of the same name.
        self.se_uart_baud = se_uart_baud or _DEFAULT_SE_UART_BAUD
        self.replace_atoc = replace_atoc

    @classmethod
    def name(cls):
        return 'alif_flash'

    @classmethod
    def capabilities(cls):
        # MRAM provisioning only: no debug/attach (that is the jlink
        # runner's job -- see board.cmake). dev_id carries --device so a
        # board.cmake board_runner_args(alif_flash --device=...) lands in
        # the right place.
        return RunnerCaps(commands={'flash'}, dev_id=True)

    @classmethod
    def dev_id_help(cls):
        return ('Alif part number whose first 5 chars match the SETOOLS '
                'global-cfg.db Part# (e.g. AE822FA0E5597LS0_HE).')

    @classmethod
    def do_add_parser(cls, parser):
        parser.add_argument(
            '--device', dest='device',
            help='Alif device/part profile (alias of -i/--dev-id).')
        parser.add_argument(
            '--setools-dir', dest='setools_dir',
            help='Path to the Alif Security Toolkit (app-release-exec-linux) '
                 'directory containing app-gen-toc + app-write-mram. '
                 'Defaults to $SETOOLS_DIR. License-gated; not shipped with '
                 'alp-sdk.')
        parser.add_argument(
            '--se-uart', dest='se_uart',
            help='SE-UART serial device app-write-mram talks to (e.g. '
                 '/dev/ttyUSB0). Defaults to $SE_UART. Host-specific.')
        parser.add_argument(
            '--app-gen-toc', dest='gen_toc', default='app-gen-toc',
            help='app-gen-toc executable name/path (default: app-gen-toc).')
        parser.add_argument(
            '--app-write-mram', dest='write_mram', default='app-write-mram',
            help='app-write-mram executable name/path '
                 '(default: app-write-mram).')
        parser.add_argument(
            '--se-uart-baud', dest='se_uart_baud',
            help='Baud rate for the pre-burn ATOC guard\'s (#2262) '
                 '`maintenance -opt getbanner`/`gettoc` queries. Defaults '
                 'to $SE_UART_BAUD, else 57600 -- matches '
                 'scripts/bench/aen/bench-env.sh.')
        parser.add_argument(
            '--replace-atoc', dest='replace_atoc', action='store_true',
            help='Override the pre-burn ATOC guard (#2262) and burn even '
                 'when a resident app entry outside this build\'s own '
                 'ALP-HE/ALP-HP section would be silently delisted, or '
                 'when the resident ATOC could not be verified. Same '
                 'spelling as flash-run.sh\'s Flow A flag -- distinct '
                 'from, and never aliased with, Flow D\'s '
                 '--atoc-unqueryable (#2027).')

    @classmethod
    def do_create(cls, cfg, args):
        # --device is an alias for the common -i/--dev-id; prefer whichever
        # was supplied (board.cmake passes --device).
        device = args.device or args.dev_id
        # SETOOLS dir + SE-UART fall back to the same env vars the bench
        # helpers use (bench-env.sh), so a host already set up for
        # flash-run.sh needs no extra flags.
        setools_dir = args.setools_dir or os.environ.get('SETOOLS_DIR')
        se_uart = args.se_uart or os.environ.get('SE_UART')
        se_uart_baud = (args.se_uart_baud or os.environ.get('SE_UART_BAUD')
                        or _DEFAULT_SE_UART_BAUD)
        return AlifFlashBinaryRunner(
            cfg, device, setools_dir=setools_dir, se_uart=se_uart,
            gen_toc=args.gen_toc, write_mram=args.write_mram,
            se_uart_baud=se_uart_baud, replace_atoc=args.replace_atoc)

    def do_run(self, command, **kwargs):
        if command != 'flash':
            raise ValueError(f'{self.name()} only supports flash, not '
                             f'{command}')

        # MEDIUM-3 review (#2262): clear any verdict a PREVIOUS run left
        # behind before this attempt reaches (or fails before reaching)
        # the guard step. Without this, a run that fails early -- no
        # SETOOLS, no zephyr.bin, a rejected ATOC window, the #2274
        # multi-domain refusal just below, any of the RuntimeErrors below
        # -- would leave the LAST run's atoc-guard.json in place, and a
        # caller reading it (tan-cli's zephyr_west_flash backend,
        # alplabai/tan-cli#1267) could mistake a stale "clear"/"replaced"
        # verdict for THIS run's own outcome. `unlink(missing_ok=True)`
        # rather than writing a "pending" status: the file's mere ABSENCE
        # is then the unambiguous signal "the guard never reached a
        # verdict for this attempt", with no schema change needed to
        # distinguish it from a real one.
        #
        # Deliberately placed BEFORE the #2274 multi-domain refusal
        # (moved here on review): unlinking this runner's OWN prior
        # output is not an MRAM/ATOC side effect -- it never touches the
        # device or stages anything -- so a #2274 refusal must still
        # clear it, the same as every other early-abort path here. Left
        # in place, a #2274 refusal would leave a PREVIOUS run's
        # clear/replaced verdict looking like this attempt's own result.
        if self.cfg.build_dir:
            (Path(self.cfg.build_dir) / 'alif_flash' / 'atoc-guard.json').unlink(
                missing_ok=True)

        # Multi-domain sysbuild refusal (#2274) -- BEFORE any staging,
        # gettoc, J-Link, or burn side effect. See the module docstring
        # section of the same name and `_refuse_if_multi_domain_sysbuild`.
        _refuse_if_multi_domain_sysbuild(self.cfg.build_dir)
        # `atoc-before.txt` is deliberately NOT removed here (nit review,
        # #2262 -- considered and decided against, not an oversight): its
        # whole audit-trail value is being the LAST successfully-read
        # resident-ATOC transcript, whether or not THIS run's guard ever
        # attempts a new read (see `_run_atoc_guard`'s own module-docstring
        # note on why bench-env.sh's transcript retention is deliberate,
        # not a leak). A run that fails before the guard step captured
        # nothing new to report, so the stale-but-real transcript from the
        # last run that DID read the board is still useful evidence and
        # actively worth keeping -- unlike `atoc-guard.json`, whose only
        # job is a per-ATTEMPT yes/no signal with no standalone value once
        # stale.

        if not self.setools_dir:
            raise RuntimeError(
                'The Alif Security Toolkit (SETOOLS) is required to flash '
                'MRAM and is NOT redistributed by alp-sdk. Obtain it from '
                'Alif, then pass --setools-dir <...>/app-release-exec-linux '
                'or export SETOOLS_DIR. See docs/aen-bench-bringup.md.')
        setools = Path(self.setools_dir)
        gen_toc = setools / self.gen_toc
        if not gen_toc.is_file():
            raise RuntimeError(
                f"'{self.setools_dir}' does not look like a SETOOLS "
                f'app-release-exec-linux dir (no {self.gen_toc}).')
        # app-gen-toc additionally needs the `fdt` Python package; surface
        # the documented one-off setup rather than a bare ImportError from
        # the SETOOLS executable.
        if shutil.which('python3') and not _has_fdt():
            self.logger.warning(
                "the 'fdt' Python package (needed by app-gen-toc) was not "
                'found; if app-gen-toc fails, run: pip install fdt')

        if not self.se_uart:
            raise RuntimeError(
                'SE_UART is unset -- pass --se-uart <serial-device> or '
                'export SE_UART to the SE-UART (Linux /dev/ttyUSB*, macOS '
                '/dev/cu.usbserial-*, Windows/WSL passed-through COM).')

        bin_file = self.cfg.bin_file
        if not bin_file or not Path(bin_file).is_file():
            raise RuntimeError(
                'no zephyr.bin in the build directory; build the app first.')

        # Auto-detect which ATOC shape this build needs from its OWN
        # reset vector -- bench-proven 2026-07-19 that app-write-mram
        # over the SE-UART burns BOTH shapes (see module docstring), so
        # this only picks the config, it never rejects a flash.
        cpu_suffix = _cpu_suffix(self.device)
        reset_vector = _reset_vector(bin_file)
        app_shape = _select_app_shape(reset_vector, cpu_suffix)

        # #1069 ATOC-assembly guard: reject a mramAddress outside its
        # cpu_id's declared slot0 window before app-gen-toc runs (a
        # single-entry flash can't hit the sibling-overlap check, but the
        # window check alone is exactly what makes _select_app_shape's
        # window lookup above load-bearing rather than decorative).
        try:
            _aen_atoc.validate_atoc_entries({'app': app_shape})
        except _aen_atoc.AtocValidationError as exc:
            raise RuntimeError(str(exc)) from exc

        # 1. Stage the image + a per-app signed-ATOC config that keeps the
        #    factory DEVICE config and points the app entry at this build's
        #    zephyr.bin, shaped per app_shape (ITCM-load or MRAM slot0-XIP).
        name = Path(self.cfg.build_dir).name or 'alp-app'
        images_dir = setools / 'build' / 'images'
        config_dir = setools / 'build' / 'config'
        images_dir.mkdir(parents=True, exist_ok=True)
        config_dir.mkdir(parents=True, exist_ok=True)
        staged_bin = images_dir / f'{name}.bin'
        shutil.copyfile(bin_file, staged_bin)

        cfg_name = f'{name}.json'
        cfg_path = config_dir / cfg_name
        # write-text-newline-exempt: generated flash cfg in a scratch run dir
        cfg_path.write_text(
            _build_atoc_config(name, app_shape), encoding='utf-8')

        # 2. Build the signed ATOC (app-gen-toc -f <config>, run from the
        #    SETOOLS dir; its paths are relative to build/).
        rel_cfg = os.path.join('build', 'config', cfg_name)
        self.check_call([str(gen_toc), '-f', rel_cfg], cwd=str(setools))

        # 2b. Refuse a package that grew below the `atoc` band into the
        #     preset's customer `storage` region (#2234): an ITCM load image
        #     is stored inside the package, so a Flow A-shaped package is
        #     ~89 KB, not the 32 KiB the band reserves.
        #     ALP_ATOC_ALLOW_OVER_STORAGE=1 accepts it for an image that
        #     never writes `storage` (same switch as the bench scripts).
        try:
            map_text = (setools / 'build' / 'app-package-map.txt').read_text(encoding='utf-8')
            _aen_atoc.validate_package_extent(
                _aen_atoc.package_start_from_map(map_text),
                os.environ.get('ALP_ATOC_ALLOW_OVER_STORAGE') == '1')
        except (OSError, _aen_atoc.AtocValidationError) as exc:
            raise RuntimeError(str(exc)) from exc
        # 2c. Pre-burn ATOC-replace guard (#2262) -- see the module
        # docstring section of the same name. Raises RuntimeError (before
        # step 3 burns anything) on a foreign resident entry or an
        # unverified read, unless --replace-atoc was passed.
        self._run_atoc_guard(setools, {_atoc_section_name(app_shape)})

        # 3. Burn the ATOC (and, for a slot0-XIP build, the standalone app
        #    blob at its mramAddress) to MRAM over the SE-UART in one pass.
        #    -p programs; the SES auto-enters maintenance, writes, resets,
        #    and boots the image. Bench-verified 2026-07-19: a single
        #    app-write-mram run burns both blobs when the ATOC references a
        #    standalone mramAddress entry.
        #    cite: scripts/bench/aen/flash-run.sh:54  (app-write-mram -c <uart> -p)
        write_mram = setools / self.write_mram
        if not write_mram.is_file():
            raise RuntimeError(
                f'no {self.write_mram} in {self.setools_dir}.')
        self.check_call(
            [str(write_mram), '-c', self.se_uart, '-p'], cwd=str(setools))

        shape_desc = ('MRAM slot0-XIP' if 'mramAddress' in app_shape
                      else 'ITCM-load')
        # Review finding (#2262, second round): "the SES has booted the
        # image" claimed more than this method actually knows -- the only
        # write here is `app-write-mram -p` (never an erase), and the AEN
        # bench notes document the SE preferring a STALE resident slot0
        # image over a freshly written ITCM-load ATOC (the remedy is an
        # explicit erase over the SE-UART, which this runner never
        # performs). State only what is verified: the ATOC write itself
        # completed (`app-write-mram` exited zero); whether the board then
        # actually booted THIS image is not checked by `west flash` at
        # all -- confirm it over the console or PC/IPSR-over-SWD the same
        # way any other flow's boot is confirmed.
        self.logger.info(
            f"flashed '{name}' to MRAM via SETOOLS ({shape_desc} ATOC); "
            'the write completed -- boot not verified by this command '
            '(see docs/aen-bench-bringup.md\'s ATOC-replace guard section '
            'for the stale-slot0-shadowing gotcha).')

    def _run_atoc_guard(self, setools, allowed):
        '''Pre-burn ATOC-replace guard (#2262) -- read what SETOOLS
        reports is actually resident on the SE, over the SAME SE-UART
        `self.se_uart` this run is about to burn over, BEFORE
        `app-write-mram -p` replaces the whole ATOC. `allowed` is the
        single section this run itself is about to (re)write ('ALP-HE'
        or 'ALP-HP', see _atoc_section_name) -- the guard fires only on a
        GENUINELY foreign resident entry.

        The parse/decision logic (compute_query_status,
        foreign_resident_entries, decide_atoc_guard) lives in
        scripts/aen_atoc.py as the single shared implementation, ported
        from and parity-tested against bench_atoc_replace_guard in
        scripts/bench/aen/bench-env.sh (see
        tests/scripts/test_atoc_guard_parity.py) -- this method does only
        the SE-UART IO + build-dir bookkeeping,
        mirroring the "pure parser vs. IO-doing caller" split that module
        docstring's #2262 note describes.'''
        # Review finding (#2262, second round): the frozen v1 verdict
        # contract documents `transcript` as an "absolute path", but
        # `Path(cfg.build_dir) / ...` is only absolute when west's OWN
        # build_dir happens to be. `.resolve()` here guarantees it
        # regardless of how `do_run` was invoked (relative build_dir, a
        # caller that chdir'd, ...) rather than relying on that assumption.
        atoc_dir = (Path(self.cfg.build_dir) / 'alif_flash').resolve()
        atoc_dir.mkdir(parents=True, exist_ok=True)
        transcript_path = atoc_dir / 'atoc-before.txt'
        verdict_path = atoc_dir / 'atoc-guard.json'

        # Review finding (#2262, second round): bash's own guard checks
        # `[ -x "$SETOOLS_DIR/maintenance" ]`, not merely that the path
        # exists -- match it with `os.access(..., os.X_OK)` rather than
        # `.is_file()` alone, so a present-but-not-executable file (bad
        # permissions, a bad extract) is treated the same as "no
        # maintenance binary at all" rather than attempting to run it.
        maintenance = setools / 'maintenance'
        maintenance_available = maintenance.is_file() and os.access(
            maintenance, os.X_OK)
        banner_text = banner_rc = gettoc_text = gettoc_rc = None
        if maintenance_available:
            banner_text, banner_rc = _run_maintenance(
                maintenance, self.se_uart, self.se_uart_baud, 'getbanner')
            gettoc_text, gettoc_rc = _run_maintenance(
                maintenance, self.se_uart, self.se_uart_baud, 'gettoc')

        # Overwrite per run is fine here, unlike bench-env.sh's own
        # ${TMPDIR:-/tmp}/<tag>-atoc-before.<random> (alp-sdk#2026 round-2:
        # a SHARED fixed path let one bench run's write land between
        # another concurrent run's redirect and read, on a DIFFERENT
        # board). `self.cfg.build_dir` is west's own per-board build
        # directory, so two concurrent `west flash` invocations never
        # share one -- there is no analogous cross-run collision to guard
        # against here.
        _write_text_or_raise(
            transcript_path,
            _format_atoc_transcript(
                self.se_uart, self.se_uart_baud, maintenance_available,
                banner_text, banner_rc, gettoc_text, gettoc_rc))

        # Measured (alp-sdk#2538): a stalled SE-UART read exits 0 with a table
        # lacking the closing '+---+' line (printed only on the 0xa8 end packet),
        # so that closing-line rule is the primary defence; a closed port exits 1
        # and is refused by the non-zero exit.
        query_status = _aen_atoc.compute_query_status(
            maintenance_available, banner_text, banner_rc, gettoc_text,
            gettoc_rc)
        resident = _aen_atoc.parse_resident_atoc_table(gettoc_text or '')
        foreign = _aen_atoc.foreign_resident_entries(resident, allowed)
        verdict = _aen_atoc.decide_atoc_guard(
            query_status, foreign, self.replace_atoc)

        # Written BEFORE any raise below -- a refused run must still leave
        # a machine-readable verdict a caller (e.g. tan-cli's
        # zephyr_west_flash backend) can tell apart from any other `west
        # flash` failure without parsing stderr.
        _write_text_or_raise(
            verdict_path,
            json.dumps({
                'schema': 'alp-sdk.alif-flash-atoc-guard.v1',
                'status': verdict.status,
                'foreign': verdict.foreign,
                'transcript': str(transcript_path),
                'allowed': sorted(allowed),
                'query_status': query_status,
            }, indent=2) + '\n')

        # Review finding (#2262, second round): branch on the BOOLEAN
        # `verdict.refused` `decide_atoc_guard` already computes, not on an
        # enumerated set of status strings -- every test in this suite and
        # `test_atoc_guard_parity.py` asserts on `.refused`, but this was
        # the one consumer that instead fell through to a silent burn (`->
        # proceed`) for any status that was not EXACTLY 'refused-unverified'
        # or 'refused-foreign'. `.status` is used only below to pick which
        # message to raise, never to decide whether to abort.
        if not verdict.refused:
            self.logger.info(
                f'ATOC guard: {verdict.status} (transcript: {transcript_path})')
            return
        if verdict.status == 'refused-unverified':
            # BLOCKER review (#2262, third round): a query that exited 0
            # with a valid banner and parsed row(s), but whose transcript
            # carries NEITHER the '| Name |' header nor any '+---+'
            # separator, is not "a check that failed" in the same sense as
            # a missing maintenance binary or a non-zero exit -- it is a
            # shape this parser has never seen and cannot vouch for (a
            # truncation that swallowed BOTH structural markers, a
            # different SETOOLS box-drawing style, or SES boot-banner text
            # interleaved by a mid-query reset). Naming it "unverified" and
            # steering at --replace-atoc the same way a routine failure
            # would be is itself the wrong instinct here -- nobody has
            # confirmed what's resident, and --replace-atoc asserts they
            # have. Ask for the transcript instead.
            format_unrecognized = (
                banner_rc == 0
                and gettoc_rc == 0
                and resident
                and not _aen_atoc.table_has_structural_markers(gettoc_text or ''))
            if format_unrecognized:
                raise RuntimeError(
                    "the resident ATOC query via 'maintenance -c "
                    f"{self.se_uart} -opt gettoc' exited 0 with a valid SES "
                    'banner and parsed row(s), but the transcript (see '
                    f'{transcript_path}) matches no recognised gettoc table '
                    "format -- no '| Name |' header, no '+---+' separator "
                    'anywhere. This could be a genuine truncation that '
                    'swallowed BOTH structural markers, a different SETOOLS '
                    "box-drawing style this guard doesn't recognise yet, or "
                    'SES boot-banner text interleaved by a mid-query reset. '
                    'Refusing to burn rather than guess -- please file this '
                    'transcript (see docs/aen-provisioning.md) so the parser '
                    'can be taught the real shape. This is NOT the same '
                    'situation --replace-atoc is for.')
            # Minor review fix (#2262): this message's own "re-run with
            # --replace-atoc" advice is exactly the wrong instinct on a
            # pre-provisioned Alp Lab module with a stale/misconfigured
            # SE_UART -- an unverified read there is disproportionately
            # likely to be "this is a factory-provisioned module and I
            # haven't actually confirmed what's on it" rather than "SETOOLS
            # is broken", and --replace-atoc on such a module deletes the
            # factory MCUBOOT- entry (see the refused-foreign branch below)
            # with NO opportunity to see it named first, since the read
            # never succeeded.
            raise RuntimeError(
                "could not read the resident ATOC via 'maintenance -c "
                f"{self.se_uart} -opt gettoc' (see {transcript_path}). A "
                'fresh ATOC write REPLACES every app entry not in it, so '
                'writing blind risks silently delisting anything already '
                'on this board -- that is exactly how an AEN EVK bench '
                'unit lost its A32 Linux boot chain on 2026-09-07. On a '
                'pre-provisioned Alp Lab module this includes the factory '
                f"{_FACTORY_MCUBOOT_ATOC_NAME!r} MCUboot bootloader -- "
                '--replace-atoc here could delist it BLIND, with no chance '
                'to see it named first (see docs/aen-provisioning.md '
                '"0.5 If your module came from Alp Lab"). Confirm by hand '
                'what is resident, then re-run with --replace-atoc only '
                'once you know it is safe to lose.')
        if verdict.status == 'refused-foreign':
            names = ', '.join(verdict.foreign)
            if _FACTORY_MCUBOOT_ATOC_NAME in verdict.foreign:
                # HIGH-2 review (#2262): a factory MCUBOOT- entry is not
                # an ordinary foreign app you might restore later -- it is
                # the bootloader that makes this module boot at all.
                # Steer away from --replace-atoc as the "just do it"
                # remedy the generic message below invites; name the
                # supported path instead.
                raise RuntimeError(
                    'this write REPLACES every app ATOC entry not in it '
                    f'-- it does NOT merge. This board also carries: '
                    f'{names}, including the factory-provisioned '
                    f"{_FACTORY_MCUBOOT_ATOC_NAME!r} MCUboot bootloader "
                    '(zephyr/sysbuild/aen/README.md). Burning now would '
                    f'SILENTLY DELIST {_FACTORY_MCUBOOT_ATOC_NAME!r}, '
                    'leaving the module unable to boot at all until '
                    'MCUboot is reprovisioned -- this is the same class '
                    'of loss that destroyed the A32 Linux boot chain on '
                    'an AEN EVK bench unit, 2026-09-07. Load your app '
                    'WITHOUT disturbing the factory MCUboot ATOC entry '
                    'instead: a plain J-Link loadbin of your '
                    'imgtool-signed image straight to slot0, no '
                    'SETOOLS/ATOC/SE-UART at all (docs/aen-provisioning.md '
                    'section 0.5, Option B). --replace-atoc still '
                    f'overrides this refusal if you are certain -- but it '
                    f'deletes {_FACTORY_MCUBOOT_ATOC_NAME!r} and the '
                    'module will not boot until MCUboot is reprovisioned.')
            # LOW review (#2262, second round): lead with "restore first",
            # not with the destroy flag -- an earlier version of this
            # message opened its remedy sentence with "Re-run with
            # --replace-atoc", which is exactly the steer-onto-the-
            # destroy-flag failure the factory-MCUBOOT- branch above was
            # written to avoid. This branch is reached for ANY unrecognised
            # foreign name -- including a factory bootloader whose real
            # name doesn't happen to match `_FACTORY_MCUBOOT_ATOC_NAME`
            # (see that constant's own TBD note) -- so it must not
            # default to inviting deletion either.
            raise RuntimeError(
                'this write REPLACES every app ATOC entry not in it -- it '
                f'does NOT merge. This board also carries: {names}. '
                f'Writing now would SILENTLY DELIST {names} -- no error, '
                'no SES warning (this destroyed the A32 Linux boot chain '
                'on an AEN EVK bench unit, 2026-09-07). Capture/restore '
                f'{names} first (see docs/aen-provisioning.md) -- only '
                f'pass --replace-atoc once you can restore {names}, or if '
                'losing them is genuinely intended.')
        # Defensive fail-closed default: `verdict.refused` is True but
        # `.status` matches neither shape above -- a future
        # `decide_atoc_guard` status this method has no dedicated message
        # for yet. Abort rather than silently falling through to a burn,
        # which is exactly the bug the `verdict.refused` branch above
        # replaces.
        raise RuntimeError(
            f'ATOC guard refused this burn (status: {verdict.status!r}; '
            f'see {transcript_path}) -- no dedicated message is defined '
            'for this status yet; refusing rather than proceeding.')


def _has_fdt():
    '''True if the `fdt` Python package (an app-gen-toc dependency) is
    importable in this interpreter -- used only for a setup hint.'''
    try:
        import importlib.util
        return importlib.util.find_spec('fdt') is not None
    except (ImportError, ValueError):
        return False
