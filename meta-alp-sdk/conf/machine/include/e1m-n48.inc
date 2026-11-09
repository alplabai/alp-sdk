# R9A09G056N48GBG (RZ/V2N N48: GPU + ISP + security IP) delta, shared by every
# N48 SKU (E1M-V2N104..106, E1M-V2M104..106).  Datasheet R01DS0466EJ0130
# Table 1.3-1: the A55 Arm crypto extensions (aes/sha/pmull) exist only on the
# security-option parts (N45-N48); the N44 behind E1M-V2N101..103 / V2M101..103
# has none (measured: no aes/sha1/sha2/pmull in /proc/cpuinfo).
#
# `e1m-n48` is the override that gates the N48-only bits; nothing else keys
# on it.  Secure boot / the security IP proper needs the Renesas RZ MPU
# Security Package (NDA) and is not wired here.
MACHINEOVERRIDES =. "e1m-n48:"

# Kernel: the arm64 CE crypto drivers (aes/sha1/sha2/ghash).  Built in; each
# returns -ENODEV at init on a CPU without the feature, so it is inert on N44
# anyway, but it is only added on N48 so the N44 image stays as it was.
SRC_URI:append:pn-linux-renesas = " file://crypto-arm64-ce.cfg"

# Userland: the N48 tune carries `crypto` (-mcpu=cortex-a55+crypto) via the stock
# cortexa55 tune that rzv2n-evk.conf requires; nothing to add here.  NOTE the
# N44 machines get the same stock tune -- see the CHANGELOG fragment for 3119.
