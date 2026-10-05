# The package pins COMPATIBLE_MACHINE to its own EVK machine, which no Alp
# machine matches.  Every E1M-V2N/V2M machine carries the e1m-v2n101 machine
# override (conf/machine/e1m-v2*-a55.conf), so widen the match with it; the
# package's own value stays in force for its own machine.  Parsed only when
# the meta-rz-isp layer is present (BBFILES_DYNAMIC).
COMPATIBLE_MACHINE:append = "|e1m-v2n101"
