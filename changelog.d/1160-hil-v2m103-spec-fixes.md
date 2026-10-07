### Fixed — four HIL spec/runner bugs found by the first V2M103 bench run (#1160)

- **GD32 protocol checks are a floor, not a list.** `v2m103-gd32-bridge-linux`, and `*-gd32-getversion-i2c` for V2M103 and V2N101, hard-coded minor 14 or 15 and failed on a board reporting protocol 0.17.0; they now accept any 0.x with minor >= 14.
- **`ssh_command` goes to `sh -s` on stdin.** Passed as one argv string, Windows `ssh.exe` mangled `$(printf %03o "$v")`-style constructs; `run_smoke.py` now sends the command on stdin.
- **Failed specs print the board output.** The summary showed only the token verdict; it now shows the tail of the captured stdout/stderr.
- **`*-xspi-mtd` no longer passes on an empty read.** BusyBox `head` has no `-c`, so both hashes were md5 of empty input and `HIL_MTD0_STABLE` passed falsely; the specs now read with `dd` and require 4194304 bytes and a non-empty-input hash.
