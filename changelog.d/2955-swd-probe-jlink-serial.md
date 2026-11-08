### Fixed — swd_probe J-Link path selects its probe by serial and reads the transcript (#2955)

**`scripts/flash_backends/swd_probe.py` requires a probe serial for the J-Link path (`flash_args.jlink_serial` or `ALP_JLINK_SERIAL`, passed as `-SelectEmuBySN`), runs `exec DisableAutoUpdateFW` before an explicit connect instead of `-AutoConnect`, and judges success from the JLinkExe transcript instead of trusting exit code 0.** With several probes attached, the previous code could program whichever probe JLinkExe picked.
