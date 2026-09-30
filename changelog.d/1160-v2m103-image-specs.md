### Added

- `tests/hil/run_smoke.py` `ssh-run` specs may carry `ssh_command:` (a read-only shell one-liner run over plain ssh, no example binary) to check the running Linux image. Four E1M-V2M103 specs use it: GD32 bridge protocol 0.14.0 in dmesg, DX-M1 `/dev/dxrt0` + `dxrt.service`, eMMC root + no ALSA card, no unexpected failed systemd units (`systemd-networkd-wait-online` excluded until #2450). Bench facts from 2026-09-29; the specs themselves are not yet run through the runner (#1160).
