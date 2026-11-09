# R9A09G056N48GBG (RZ/V2N N48: GPU + ISP + security IP) delta, shared by every
# N48 SKU (E1M-V2N104..106, E1M-V2M104..106).
#
# `e1m-n48` is the override that marks an N48 machine; the rz-security glue's
# N48 guard keys on it.
# It is the hook for the follow-up that needs the Renesas RZ MPU Security
# Package (NDA): secure boot and the rest of the security IP.
#
# Crypto: the arm64 CE kernel drivers (aes/sha1/sha2/ghash) are already built
# into the shared linux-renesas config for every RZ/V2N machine.  On N44
# (E1M-V2N101..103 / V2M101..103) the CPU has no crypto extension (measured: no
# aes/sha/pmull in /proc/cpuinfo) so they do not bind; on N48 (datasheet
# R01DS0466EJ0130 Table 1.3-1) they are expected to bind -- build-unverified,
# no N48 unit benched.
#
# Tune: the stock cortexa55 tune already gives EVERY RZ/V2N machine, N44
# included, -mcpu=cortex-a55+crypto.  Not changed here; tracked in #3120.
MACHINEOVERRIDES =. "e1m-n48:"

# Opts the N48 into the Renesas RZ MPU Security Package glue
# (dynamic-layers/rz-security, docs/v2n-security.md).  Inert unless that layer
# is in bblayers: without it nothing reads the feature.
MACHINE_FEATURES:append = " alp-security"
