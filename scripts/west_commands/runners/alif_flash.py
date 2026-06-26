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

# ATOC load/boot parameters for the M55-HE (RTSS-HE) target. Transcribed
# from the proven bench recipe in scripts/bench/aen/flash-run.sh (the
# Flow A ITCM-load single-blob ATOC) and docs/aen-provisioning.md.
#
#   loadAddress 0x58000000 = the M55-HE ITCM *global* alias the SES loads
#   the embedded app image to before booting it.
#       cite: scripts/bench/aen/flash-run.sh:46  ("loadAddress": "0x58000000")
#       cite: docs/aen-provisioning.md:166        ("loadAddress": "0x58000000")
#   cpu_id "M55_HE" + flags ["load","boot"] + signed:true.
#       cite: scripts/bench/aen/flash-run.sh:45-46
_HE_LOAD_ADDRESS = '0x58000000'
_HE_CPU_ID = 'M55_HE'

# slot0-XIP (two-blob) variant: an app LINKED INTO MRAM slot0 that
# overflows ITCM (e.g. a real NPU model). The ATOC then boots the app
# where it already sits in MRAM (mramAddress) rather than embedding it.
#   mramAddress 0x80010000 = MRAM base 0x80000000 + slot0 offset 0x10000.
#       cite: scripts/bench/aen/flash-jlink-mramxip.sh:45  (APP_ADDR=0x80010000)
#       cite: scripts/bench/aen/flash-jlink-mramxip.sh:67  ("mramAddress": "0x80010000")
#   flags ["boot"] only (NOT load -- the SE boots it in place); the slot0
#   reset vector then lands in MCUboot/the app at VTOR 0x80010800.
_HE_SLOT0_MRAM_ADDRESS = '0x80010000'


class AlifFlashBinaryRunner(ZephyrBinaryRunner):
    '''Flash an AEN801 image to MRAM with the Alif SETOOLS (Flow A).'''

    def __init__(self, cfg, device, setools_dir=None, se_uart=None,
                 mram_xip=False, gen_toc='app-gen-toc',
                 write_mram='app-write-mram'):
        super().__init__(cfg)
        # --device first 5 chars must match the SETOOLS global-cfg.db
        # Part# ("AE822..."); the runner only forwards it for the staged
        # config + diagnostics. cite: board.cmake comment + :39.
        self.device = device
        self.setools_dir = setools_dir
        self.se_uart = se_uart
        self.mram_xip = mram_xip
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
            '--mram-xip', dest='mram_xip', default=False,
            action='store_true',
            help='Reject the flash with a pointer to the two-blob bench Flow '
                 'D helper. A slot0-linked app that overflows ITCM (mramAddress '
                 f'{_HE_SLOT0_MRAM_ADDRESS}, flags ["boot"]) needs the raw app '
                 'blob written to slot0 over the J-Link Alif MRAM loader, which '
                 'this SE-UART-only runner does not drive; use '
                 'scripts/bench/aen/flash-jlink-mramxip.sh instead.')
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
            mram_xip=bool(args.mram_xip), gen_toc=args.gen_toc,
            write_mram=args.write_mram)

    def do_run(self, command, **kwargs):
        if command != 'flash':
            raise ValueError(f'{self.name()} only supports flash, not '
                             f'{command}')

        # A slot0-XIP app is NOT embedded in the ATOC (flags ["boot"],
        # mramAddress) -- the ATOC only tells the SE to boot whatever
        # already sits at 0x80010000. Provisioning it therefore needs the
        # standalone app blob written to slot0 *as well as* the ATOC. This
        # SE-UART-only runner burns just the ATOC (app-write-mram -p), so a
        # --mram-xip flash here would leave slot0 stale and the SE would
        # boot a garbage image. Writing the raw app blob to an MRAM address
        # needs the J-Link built-in Alif MRAM loader (bench Flow D), which
        # this runner deliberately does not use (see capabilities()).
        if self.mram_xip:
            raise RuntimeError(
                'slot0-XIP provisioning needs two blobs (the app at '
                f'{_HE_SLOT0_MRAM_ADDRESS} AND the signed ATOC), and the raw '
                'app blob must be written over the J-Link Alif MRAM loader '
                'which this SE-UART-only runner does not drive. Use the '
                'two-blob bench Flow D helper instead: '
                'scripts/bench/aen/flash-jlink-mramxip.sh <build-dir> (see '
                'docs/aen-bench-bringup.md, Flow D).')

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

        # 1. Stage the image + a per-app signed-ATOC config that keeps the
        #    factory DEVICE config and points the app entry at this build's
        #    zephyr.bin. This mirrors flash-run.sh:41-48 (ITCM load) exactly.
        name = Path(self.cfg.build_dir).name or 'alp-app'
        images_dir = setools / 'build' / 'images'
        config_dir = setools / 'build' / 'config'
        images_dir.mkdir(parents=True, exist_ok=True)
        config_dir.mkdir(parents=True, exist_ok=True)
        staged_bin = images_dir / f'{name}.bin'
        shutil.copyfile(bin_file, staged_bin)

        cfg_name = f'{name}.json'
        cfg_path = config_dir / cfg_name
        cfg_path.write_text(self._atoc_config(name), encoding='utf-8')

        # 2. Build the signed ATOC (app-gen-toc -f <config>, run from the
        #    SETOOLS dir; its paths are relative to build/).
        rel_cfg = os.path.join('build', 'config', cfg_name)
        self.check_call([str(gen_toc), '-f', rel_cfg], cwd=str(setools))

        # 3. Burn the ATOC to MRAM over the SE-UART. -p programs; the SES
        #    auto-enters maintenance, writes, resets, and boots the image.
        #    cite: scripts/bench/aen/flash-run.sh:54  (app-write-mram -c <uart> -p)
        write_mram = setools / self.write_mram
        if not write_mram.is_file():
            raise RuntimeError(
                f'no {self.write_mram} in {self.setools_dir}.')
        self.check_call(
            [str(write_mram), '-c', self.se_uart, '-p'], cwd=str(setools))

        self.logger.info(
            f"flashed '{name}' to MRAM via SETOOLS (ITCM-load ATOC); "
            'the SES has booted the image.')

    def _atoc_config(self, name):
        '''Return the signed-ATOC JSON for this build.

        DEVICE keeps the factory device config (signed); the app entry is
        signed + tagged for the M55-HE and embedded ITCM-load (loadAddress
        0x58000000, flags load+boot), transcribed verbatim from the bench
        recipe. cite: scripts/bench/aen/flash-run.sh:42-48
        '''
        app_entry = (
            f'                 "cpu_id": "{_HE_CPU_ID}", '
            f'"loadAddress": "{_HE_LOAD_ADDRESS}", '
            '"flags": ["load", "boot"] }')
        return (
            '{\n'
            '    "DEVICE":  { "disabled": false, '
            '"binary": "app-device-config.json", "version": "0.5.00", '
            '"signed": true },\n'
            '    "ALP-HE":  { "disabled": false, '
            f'"binary": "{name}.bin", "version": "1.0.0", "signed": true,\n'
            f'{app_entry}\n'
            '}\n')


def _has_fdt():
    '''True if the `fdt` Python package (an app-gen-toc dependency) is
    importable in this interpreter -- used only for a setup hint.'''
    try:
        import importlib.util
        return importlib.util.find_spec('fdt') is not None
    except (ImportError, ValueError):
        return False
