### Fixed — zephyr-build: sysbuild key path, wider patch guard, drop dead PERIPH knobs (#2949, #2950, #2951, #2985)

AEN sysbuild: the curated sysbuild.conf set a repo-relative
SB_CONFIG_BOOT_SIGNATURE_KEY_FILE, which MCUboot resolves against
MCUBOOT_DIR and the app signer against APPLICATION_CONFIG_DIR/WEST_TOPDIR,
so neither side found the key. The base no longer carries it; the
orchestrator passes the shared dev key as an absolute ${SDK_ROOT}-anchored
-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE when board.yaml boot: names no key_file.
An explicit boot.signing.key_file that is not a file (relative to the
board.yaml directory) now fails the plan with OrchestratorError naming the
resolved path. The iot-fleet-ota and production-deployment examples drop
boot.signing and use the default key; their READMEs match.

alp_patch_guard.cmake now also covers the patches whose absence builds clean
and misbehaves at runtime: mcuboot/0001, 0002, 0004 (AEN MRAM), zephyr/0005
(RZ TINT status clear) and hal_alif/0003-0013 (ISP). The helper gains an ALL
mode and quoted multi-word markers; tests cover missing-patch failures.

Remove ALP_SDK_PERIPH_{PWM,I2S,CAN,COUNTER,QENC,RTC,WDT}: nothing consumed
them. Document that the per-library accelerator knobs and the V2N TRNG
entropy option are not yet wired into the build.
