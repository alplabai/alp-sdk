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
- A slot0-XIP app (reset vector 0x8001xxxx -- e.g. a build that
  overflows ITCM, such as a real NPU model) gets a standalone
  ("boot"-only) ATOC entry at mramAddress 0x80010000; the SE boots it
  in place.

Bench-verified 2026-07-19: a single ``app-write-mram -c <uart> -p`` run
over the SE-UART burns BOTH shapes -- for the slot0-XIP case it burns
the standalone app blob to 0x80010000 AND the signed ATOC in one pass
(two ``COMMAND_BURN_MRAM`` phases, both confirmed by read-back:
byte-exact at 0x80010000, reset vector 0x80011F15, core PC 0x80016048
running from slot0). There is therefore no separate J-Link-only
requirement for slot0 provisioning; the two-blob bench Flow D helper
(``scripts/bench/aen/flash-jlink-mramxip.sh``) remains available as an
alternative, not a prerequisite.

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

import os
import shutil
from pathlib import Path

from runners.core import RunnerCaps, ZephyrBinaryRunner

# ATOC app-entry shapes. Both burn identically over the SE-UART
# (app-write-mram -c <uart> -p); which one applies to a given build is
# auto-detected from its reset vector (see _select_app_shape below), not
# a user flag -- bench-verified 2026-07-19 (see module docstring).
#
# ITCM-load (embedded): the app is linked to run from the M55 ITCM
# global alias; the SE copies it there before booting.
#   loadAddress 0x58000000 (M55-HE) / 0x50000000 (M55-HP).
#       cite: scripts/bench/aen/flash-run.sh:46  ("loadAddress": "0x58000000")
#       cite: docs/aen-provisioning.md:166        ("loadAddress": "0x58000000")
#   flags ["load", "boot"], signed:true.
#       cite: scripts/bench/aen/flash-run.sh:45-46
#
# MRAM slot0 XIP (standalone): an app LINKED INTO MRAM slot0 (e.g. one
# that overflows ITCM, such as a real NPU model). The ATOC only tells
# the SE to boot it where it already sits (mramAddress), not to load it.
#   mramAddress 0x80010000 = MRAM base 0x80000000 + slot0 offset 0x10000
#   -- MUST be the full address; the bare 0x10000 offset gives SETOOLS
#   "Invalid Global Address" (bench-pinned).
#       cite: scripts/bench/aen/flash-jlink-mramxip.sh:45  (APP_ADDR=0x80010000)
#       cite: scripts/bench/aen/flash-jlink-mramxip.sh:67  ("mramAddress": "0x80010000")
#   flags ["boot"] only (NOT load -- the SE boots it in place); the slot0
#   reset vector then lands in MCUboot/the app at VTOR 0x80010800.
_CPU_PROFILES = {
    'HE': ('M55_HE', '0x58000000'),
    'HP': ('M55_HP', '0x50000000'),
}
_DEFAULT_CPU_SUFFIX = 'HE'  # boards that omit --device were always HE-only
_SLOT0_MRAM_ADDRESS = '0x80010000'


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


_SLOT0_REGION_END = 0x80580000  # System MRAM base (bench-confirmed); App
                                 # MRAM slot0 XIP images sit below this.


def _select_app_shape(reset_vector, cpu_suffix):
    '''Map a build's reset vector to its ATOC app-entry shape. Both
    shapes burn identically over the SE-UART (see module docstring);
    this only selects the config, it never rejects a flash (except a
    core mismatch -- see the TCM branch below).'''
    if 0x80010000 <= reset_vector < _SLOT0_REGION_END:
        # Shared App-MRAM slot0 XIP region (0x80010000, the load offset,
        # up to System MRAM at 0x80580000). BOTH cores link a slot0 app
        # to this same region, so the vector can't tell HE from HP here
        # -- --device is the only signal for cpu_id in this branch.
        cpu_id, _unused_load_address = _CPU_PROFILES[cpu_suffix]
        return {
            'cpu_id': cpu_id,
            'mramAddress': _SLOT0_MRAM_ADDRESS,
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
        f'slot0-XIP image in App MRAM (0x80010000..0x{_SLOT0_REGION_END:08x}) '
        'or an ITCM-load image (0x58xxxxxx M55-HE / 0x50xxxxxx M55-HP). '
        'Refusing to burn a mismatched image (see docs/aen-provisioning.md).')


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
    section = f'ALP-{app_shape["cpu_id"].split("_")[1]}'
    return (
        '{\n'
        '    "DEVICE":  { "disabled": false, '
        '"binary": "app-device-config.json", "version": "0.5.00", '
        '"signed": true },\n'
        f'    "{section}":  {{ "disabled": false, '
        f'"binary": "{name}.bin", "version": "1.0.0", "signed": true,\n'
        f'{app_entry}\n'
        '}\n')


class AlifFlashBinaryRunner(ZephyrBinaryRunner):
    '''Flash an AEN801 image to MRAM with the Alif SETOOLS (Flow A).'''

    def __init__(self, cfg, device, setools_dir=None, se_uart=None,
                 gen_toc='app-gen-toc', write_mram='app-write-mram'):
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
        return AlifFlashBinaryRunner(
            cfg, device, setools_dir=setools_dir, se_uart=se_uart,
            gen_toc=args.gen_toc, write_mram=args.write_mram)

    def do_run(self, command, **kwargs):
        if command != 'flash':
            raise ValueError(f'{self.name()} only supports flash, not '
                             f'{command}')

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
        cfg_path.write_text(
            _build_atoc_config(name, app_shape), encoding='utf-8')

        # 2. Build the signed ATOC (app-gen-toc -f <config>, run from the
        #    SETOOLS dir; its paths are relative to build/).
        rel_cfg = os.path.join('build', 'config', cfg_name)
        self.check_call([str(gen_toc), '-f', rel_cfg], cwd=str(setools))

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
        self.logger.info(
            f"flashed '{name}' to MRAM via SETOOLS ({shape_desc} ATOC); "
            'the SES has booted the image.')


def _has_fdt():
    '''True if the `fdt` Python package (an app-gen-toc dependency) is
    importable in this interpreter -- used only for a setup hint.'''
    try:
        import importlib.util
        return importlib.util.find_spec('fdt') is not None
    except (ImportError, ValueError):
        return False
