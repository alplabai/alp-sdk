# SPDX-License-Identifier: Apache-2.0
# Fail closed: the Renesas security package's switches (TRUSTED_BOARD_BOOT,
# ENABLE_SPD_OPTEE, ENABLE_RZ_SCE) only do something when the rz-security layer
# is in bblayers AND the MACHINE has the alp-security feature (an N48).  Set
# anywhere else they would be ignored and the build would quietly produce
# UNSIGNED firmware (or fail late in the wic step on an N44), so refuse.
# Inherited globally from conf/distro/alp.conf; docs/v2n-security.md.
# The package itself treats a switch as on when '1' is one of its
# whitespace-separated words (bb.utils.contains), so test the same way; any
# other non-empty value (yes, true, y) is refused because the package would
# silently ignore it.
python () {
    on = []
    for v in ('TRUSTED_BOARD_BOOT', 'ENABLE_SPD_OPTEE', 'ENABLE_RZ_SCE'):
        words = (d.getVar(v) or '').split()
        if any(w not in ('0', '1') for w in words):
            bb.fatal("%s = \"%s\": use \"1\" or \"0\"; the Renesas security package ignores any other value." % (v, d.getVar(v)))
        if '1' in words:
            on.append(v)
    if not on:
        return
    if 'rz-security' in (d.getVar('BBFILE_COLLECTIONS') or '').split() and \
       'alp-security' in (d.getVar('MACHINE_FEATURES') or '').split():
        return
    bb.fatal("%s = \"1\" but the Renesas security package is not active for MACHINE %s (needs the rz-security layer in bblayers and an N48 MACHINE with the alp-security feature). Refusing rather than building unsigned firmware." % (', '.join(on), d.getVar('MACHINE')))
}
