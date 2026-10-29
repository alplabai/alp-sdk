### Fixed — hand plain-CMake builds the SoM's SoC capability profile (#2912)

<alp/soc_caps.h> keys on CONFIG_ALP_SOC_*, which only Zephyr's Kconfig
defined. Yocto (A55) and baremetal builds therefore always took the
permissive "unknown" branch: ALP_SOC_REF_STR was "unknown", so backends
registered for an exact silicon_ref (the V2N UIO/MHU rpc backend,
renesas:rzv2n:n44) never matched, and every ALP_HAS() was true at once
(ALP_HAS(HELIUM_MVE) on a Cortex-A55).

The top-level CMakeLists.txt gains an ALP_SOC cache variable that
defines CONFIG_<ALP_SOC>=1 on the public alp_sdk_headers target (so apps
and the installed .pc see the same profile), rejecting a malformed
value. alp_project.py --emit cmake-args emits -DALP_SOC=<symbol> and
--emit yocto-conf emits ALP_SDK_SOC, which the alp-sdk recipe forwards.
The symbol comes from the same silicon-kconfig.json registry as the
Zephyr fragment. Emit snapshots, generator docs and soc_caps.h header
comment regenerated; tests prove the define reaches a plain-CMake
configure.
