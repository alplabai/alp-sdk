# Copyright (c) 2026 Alp Lab AB
#
# SPDX-License-Identifier: Apache-2.0

'''west runner: flash the RZ/V2N Cortex-M33 system-manager image with a
single-stage raw write to xSPI ``/dev/mtd1`` over SSH to the booted A55
Linux, so plain ``west flash`` runs the same bench-proven ``flash_erase``
+ ``mtd_debug write`` + md5-readback recipe used by hand on the bench.

Mechanism (design-decided; matches the bench-proven manual recipe under
"CM33 (Zephyr) firmware -> mtd1" in the maintainer bench notes) -- this
is a SINGLE-STAGE raw write, NOT a FIP rebuild: the alp BL2 raw-loads
the CM33 firmware
from a FIXED xSPI address (default mtd1 offset ``0x1a0000``, length cap
``0x30000``) into SRAM0 ``0x08000000`` with the reset vector at
``0x08003000``. One runner serves both the E1M-V2N101 and E1M-V2M101
boards (same PCB / BL2 / mtd layout).

Steps: (1) image sanity on the local ``zephyr.bin`` -- refuse anything
that isn't linked for this exact SRAM0/pad window (an oversized image
would silently truncate under the BL2's fixed length cap -- data-loss
grade, this guard is mandatory); (2) stage a zero-padded local copy;
(3) ``scp`` it to the board's ``/tmp`` and md5-compare local vs remote;
(4) ``flash_erase`` + ``mtd_debug write`` + ``mtd_debug read`` + a
byte-exact md5 readback (never report success without it); (5) reboot
the board in a SEPARATE ssh invocation unless ``--no-reboot``.

Why this lives in alp-sdk and not upstream Zephyr
--------------------------------------------------
Same rationale as ``alif_flash.py``: upstream Zephyr's ``runners``
package has no RZ/V2N mtd-flash runner, and ADR-0017 says we ride *over*
the vendor SDK rather than patch it. This module is surfaced as a Zephyr
*module runner* via ``zephyr/module.yml``'s ``runners:`` list. It imports
only ``runners.core`` (Zephyr's stable runner base) + the Python stdlib
and shells to the host's ``ssh``/``scp``.

Setup (one-off, per host)
--------------------------
- ``--host`` (or the ``ALP_V2N_SSH_HOST`` environment variable) is
  REQUIRED and has NO default -- this is a public repo and a bench IP
  must never be committed to it.
- The board must already be booted to Linux with ``root`` SSH access
  (see the bench notes); ``--ssh-user`` defaults to ``root``.
- ``flash_erase``, ``mtd_debug``, and ``md5sum`` must be present on the
  board's root filesystem; this runner preflight-checks for them and
  errors clearly (naming which are missing) rather than failing deep
  into the burn with a cryptic "command not found".

WSL-vs-Windows ``known_hosts`` gotcha
--------------------------------------
WSL's OpenSSH client and a native Windows OpenSSH client keep SEPARATE
``known_hosts`` files (``~/.ssh/known_hosts`` inside the WSL distro vs
the Windows user profile's ``.ssh\\known_hosts``). Trusting the board's
host key from one environment does NOT carry over to the other -- the
first ``ssh``/``scp`` from whichever side you run this runner from will
prompt to accept the host key interactively, and a non-interactive west
invocation will just hang on that prompt. Connect once by hand from the
same shell (WSL or Windows) you'll run ``west flash`` from, accept the
prompt, and the runner will proceed non-interactively after that.
'''

import math
import os
import shutil
from pathlib import Path

from runners.core import RunnerCaps, ZephyrBinaryRunner

_SRAM0_BASE = 0x08000000
_ERASE_BLOCK = 4096  # xSPI NOR erase block size (bench-confirmed).

_DEFAULT_MTD = '/dev/mtd1'
_DEFAULT_OFFSET = 0x1a0000
_DEFAULT_PAD = 0x3000
_DEFAULT_MAX_SIZE = 0x30000
_DEFAULT_SSH_USER = 'root'

_REMOTE_STAGE_PATH = '/tmp/m33_fw.bin'
_REMOTE_READBACK_PATH = '/tmp/m33_fw_readback.bin'
_REQUIRED_REMOTE_TOOLS = ('flash_erase', 'mtd_debug', 'md5sum')


def _read_vectors(bin_path):
    '''Read the little-endian (SP, reset-PC) pair out of the first 8
    bytes of a raw zephyr.bin -- the Cortex-M vector table's initial
    stack pointer and 2nd entry (reset vector).'''
    with open(bin_path, 'rb') as f:
        header = f.read(8)
    if len(header) < 8:
        raise RuntimeError(
            f"'{bin_path}' is only {len(header)} bytes -- too small to "
            'contain a Cortex-M vector table (need >= 8 bytes for the '
            'initial SP + reset vector).')
    sp = int.from_bytes(header[0:4], 'little')
    pc = int.from_bytes(header[4:8], 'little')
    return sp, pc


def _validate_image(sp, pc, size, pad, max_size):
    '''Refuse anything that isn't a legal image for this BL2's raw-load
    window. All three checks are data-loss-grade: the BL2 loads exactly
    ``max_size`` bytes from a fixed flash offset into a fixed SRAM
    window, so a wrong-linked or oversized image is silently accepted by
    the flash step and only fails (or worse, half-boots) after the fact.
    '''
    if (sp & 0xFF000000) != _SRAM0_BASE:
        raise RuntimeError(
            f'initial SP 0x{sp:08x} is not in SRAM0 (0x08xxxxxx) -- this '
            "build isn't linked for the RZ/V2N CM33 raw-load window; "
            'refusing to flash.')
    pc_lo = _SRAM0_BASE + pad
    pc_hi = _SRAM0_BASE + pad + max_size
    if not (pc_lo <= pc < pc_hi):
        raise RuntimeError(
            f'reset vector 0x{pc:08x} is outside the BL2 raw-load window '
            f'0x{pc_lo:08x}..0x{pc_hi:08x} (SRAM0 base + --pad + '
            '--max-size); refusing to flash.')
    if pad + size > max_size:
        raise RuntimeError(
            f'padded image size 0x{pad + size:x} (--pad 0x{pad:x} + '
            f'zephyr.bin 0x{size:x}) exceeds --max-size 0x{max_size:x} -- '
            'the BL2 loads exactly --max-size bytes from flash, so an '
            'oversized image would be silently truncated. Refusing to '
            'flash.')


def _blocks_for_size(size, erase_block=_ERASE_BLOCK):
    '''Number of erase blocks needed to cover ``size`` bytes -- MUST be
    sized from the actual (padded) image, not a hardcoded constant: an
    under-erase leaves old NOR bits un-cleared under the tail of a
    bigger new image and silently corrupts it (bench-pinned).'''
    return math.ceil(size / erase_block)


def _missing_tools(preflight_output):
    '''Parse the ``MISSING:<tool>`` lines emitted by the preflight probe
    (see do_run) into a list of missing remote tool names.'''
    missing = []
    for line in preflight_output.splitlines():
        line = line.strip()
        if line.startswith('MISSING:'):
            missing.append(line[len('MISSING:'):])
    return missing


def _preflight_probe_cmd(tools):
    '''Shell one-liner that prints ``MISSING:<tool>`` for each tool in
    ``tools`` not found on the remote $PATH.'''
    checks = ' '.join(
        f'command -v {t} >/dev/null 2>&1 || echo MISSING:{t};'
        for t in tools)
    return f'sh -c "{checks}"'


class Rzv2nMtdFlashBinaryRunner(ZephyrBinaryRunner):
    '''Flash the RZ/V2N CM33 image to xSPI mtd1 over SSH (single-stage
    raw write, no FIP rebuild). Serves both E1M-V2N101 and E1M-V2M101
    (same PCB / BL2 / mtd layout).'''

    def __init__(self, cfg, host, ssh_user=_DEFAULT_SSH_USER,
                 mtd=_DEFAULT_MTD, offset=_DEFAULT_OFFSET,
                 pad=_DEFAULT_PAD, max_size=_DEFAULT_MAX_SIZE,
                 reboot=True):
        super().__init__(cfg)
        self.host = host
        self.ssh_user = ssh_user
        self.mtd = mtd
        self.offset = offset
        self.pad = pad
        self.max_size = max_size
        self.reboot = reboot

    @classmethod
    def name(cls):
        return 'rzv2n_mtd_flash'

    @classmethod
    def capabilities(cls):
        return RunnerCaps(commands={'flash'})

    @classmethod
    def do_add_parser(cls, parser):
        parser.add_argument(
            '--host', dest='host',
            help='SSH host/IP of the booted RZ/V2N A55 Linux. REQUIRED; '
                 'no default (never commit a bench IP to this public '
                 'repo). Falls back to $ALP_V2N_SSH_HOST.')
        parser.add_argument(
            '--ssh-user', dest='ssh_user', default=_DEFAULT_SSH_USER,
            help=f'SSH user on the board (default: {_DEFAULT_SSH_USER}).')
        parser.add_argument(
            '--mtd', dest='mtd', default=_DEFAULT_MTD,
            help=f'Target mtd device on the board (default: {_DEFAULT_MTD}).')
        parser.add_argument(
            '--offset', dest='offset', type=lambda s: int(s, 0),
            default=_DEFAULT_OFFSET,
            help='Byte offset into --mtd where the CM33 image is raw-'
                 f'loaded from (default: 0x{_DEFAULT_OFFSET:x}).')
        parser.add_argument(
            '--pad', dest='pad', type=lambda s: int(s, 0),
            default=_DEFAULT_PAD,
            help='Zero-pad length prepended to zephyr.bin so the reset '
                 f'vector lands where the BL2 expects it (default: '
                 f'0x{_DEFAULT_PAD:x}).')
        parser.add_argument(
            '--max-size', dest='max_size', type=lambda s: int(s, 0),
            default=_DEFAULT_MAX_SIZE,
            help='Total bytes the BL2 raw-loads from --offset -- the '
                 f'hard cap on --pad + zephyr.bin (default: '
                 f'0x{_DEFAULT_MAX_SIZE:x}).')
        parser.add_argument(
            '--no-reboot', dest='reboot', action='store_false',
            help='Skip the post-flash reboot (image is burned and '
                 'readback-verified, but the CM33 stays on its old '
                 'image until the next SoC reset).')

    @classmethod
    def do_create(cls, cfg, args):
        host = args.host or os.environ.get('ALP_V2N_SSH_HOST')
        return Rzv2nMtdFlashBinaryRunner(
            cfg, host, ssh_user=args.ssh_user, mtd=args.mtd,
            offset=args.offset, pad=args.pad, max_size=args.max_size,
            reboot=args.reboot)

    # -- ssh/scp plumbing ------------------------------------------------

    def _remote(self, cmd):
        '''Run ``cmd`` on the board over ssh and return its stdout.'''
        return self.check_output(
            ['ssh', f'{self.ssh_user}@{self.host}', cmd]).decode()

    def _remote_call(self, cmd):
        '''Run ``cmd`` on the board over ssh, streaming output (used for
        the burn steps, where we want the tool's progress visible).'''
        self.check_call(['ssh', f'{self.ssh_user}@{self.host}', cmd])

    def do_run(self, command, **kwargs):
        if command != 'flash':
            raise ValueError(f'{self.name()} only supports flash, not '
                              f'{command}')
        if not self.host:
            raise RuntimeError(
                'no SSH host set -- pass --host <ip> or export '
                'ALP_V2N_SSH_HOST to the booted RZ/V2N A55 Linux. This '
                'is deliberately unset by default: see the module '
                'docstring (never ship a bench IP in this public repo).')
        if not shutil.which('ssh') or not shutil.which('scp'):
            raise RuntimeError(
                'ssh/scp not found on this host -- required to flash '
                'the RZ/V2N CM33 image over the network.')

        bin_file = self.cfg.bin_file
        if not bin_file or not Path(bin_file).is_file():
            raise RuntimeError(
                'no zephyr.bin in the build directory; build the app '
                'first.')

        # 1. Image sanity -- data-loss-grade guard, see _validate_image.
        size = Path(bin_file).stat().st_size
        sp, pc = _read_vectors(bin_file)
        _validate_image(sp, pc, size, self.pad, self.max_size)

        # 2. Stage a zero-padded local copy.
        staged = Path(self.cfg.build_dir) / 'm33_fw.bin'
        with open(staged, 'wb') as out, open(bin_file, 'rb') as src:
            out.write(b'\x00' * self.pad)
            out.write(src.read())
        padded_size = self.pad + size

        # 3. Transfer + verify (md5 local vs remote).
        self.logger.info(
            f'rzv2n_mtd_flash: staging {staged.name} '
            f'(0x{padded_size:x} bytes) to '
            f'{self.ssh_user}@{self.host}:{_REMOTE_STAGE_PATH}')
        self.check_call(
            ['scp', str(staged),
             f'{self.ssh_user}@{self.host}:{_REMOTE_STAGE_PATH}'])
        local_md5 = _md5_file(staged)
        remote_md5_out = self._remote(f'md5sum {_REMOTE_STAGE_PATH}')
        remote_md5 = remote_md5_out.split()[0]
        if remote_md5 != local_md5:
            raise RuntimeError(
                f'transfer verification failed: local md5 {local_md5} != '
                f'remote md5 {remote_md5} for {_REMOTE_STAGE_PATH}.')

        # Preflight: the required mtd tools must exist on the board.
        missing = _missing_tools(
            self._remote(_preflight_probe_cmd(_REQUIRED_REMOTE_TOOLS)))
        if missing:
            raise RuntimeError(
                'missing required tool(s) on the board: '
                f'{", ".join(missing)} -- install them on the target '
                'rootfs before flashing.')

        # 4. Burn: erase, write, then a byte-exact readback compare.
        # Never report success without the readback -- a write that
        # "succeeded" per exit code but landed on stale/under-erased NOR
        # is still a corrupt image (bench-pinned).
        blocks = _blocks_for_size(padded_size)
        self.logger.info(
            f'rzv2n_mtd_flash: erasing {blocks} block(s) of {self.mtd} '
            f'at 0x{self.offset:x}')
        self._remote_call(
            f'flash_erase -q {self.mtd} 0x{self.offset:x} {blocks}')
        self._remote_call(
            f'mtd_debug write {self.mtd} 0x{self.offset:x} '
            f'{padded_size} {_REMOTE_STAGE_PATH}')
        self._remote_call(
            f'mtd_debug read {self.mtd} 0x{self.offset:x} '
            f'{padded_size} {_REMOTE_READBACK_PATH}')
        readback_md5_out = self._remote(
            f'md5sum {_REMOTE_READBACK_PATH} {_REMOTE_STAGE_PATH}')
        readback_md5s = {
            line.split()[1]: line.split()[0]
            for line in readback_md5_out.splitlines() if line.strip()}
        if (readback_md5s.get(_REMOTE_READBACK_PATH) !=
                readback_md5s.get(_REMOTE_STAGE_PATH)):
            raise RuntimeError(
                'flash readback verification failed: '
                f'{self.mtd}@0x{self.offset:x} does not byte-match the '
                'staged image after write -- do NOT trust this flash. '
                f'({readback_md5s})')
        self.logger.info(
            f'rzv2n_mtd_flash: byte-exact readback confirmed at '
            f'{self.mtd}@0x{self.offset:x} (0x{padded_size:x} bytes)')

        # 5. Reboot -- a SEPARATE ssh invocation. A `reboot -f`
        # backgrounded at the end of an `&&` chain dies with the ssh
        # session it's chained under; this must be its own connection
        # (bench-pinned).
        if self.reboot:
            self._remote_call(
                'setsid sh -c "sleep 1; reboot -f" </dev/null '
                '>/dev/null 2>&1 &')
            self.logger.info(
                'rzv2n_mtd_flash: reboot issued; the CM33 image takes '
                'effect after the next full SoC reset (CM33-only reset '
                'from Linux is not available on this part).')
        else:
            self.logger.info(
                'rzv2n_mtd_flash: --no-reboot set; image is flashed and '
                'verified but not yet running.')


def _md5_file(path):
    '''MD5 of a local file, in the same lowercase-hex form md5sum
    prints, for a direct string compare against the remote hash.'''
    import hashlib
    h = hashlib.md5()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()
