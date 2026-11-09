# SPDX-License-Identifier: Apache-2.0
# Fail closed: the Renesas security package's switches (TRUSTED_BOARD_BOOT,
# ENABLE_SPD_OPTEE, ENABLE_RZ_SCE) only do something when the rz-security layer
# is in bblayers AND the MACHINE has the alp-security feature (an N48).  Set
# anywhere else they would be ignored and the build would quietly produce
# UNSIGNED firmware (or fail late in the wic step on an N44), so refuse.
# Inherited globally from conf/distro/alp.conf; docs/v2n-security.md.
python () {
    on = [v for v in ('TRUSTED_BOARD_BOOT', 'ENABLE_SPD_OPTEE', 'ENABLE_RZ_SCE') if d.getVar(v) == '1']
    if not on:
        return
    if 'rz-security' in (d.getVar('BBFILE_COLLECTIONS') or '').split() and \
       'alp-security' in (d.getVar('MACHINE_FEATURES') or '').split():
        return
    bb.fatal("%s = \"1\" but the Renesas security package is not active for MACHINE %s (needs the rz-security layer in bblayers and an N48 MACHINE with the alp-security feature). Refusing rather than building unsigned firmware." % (', '.join(on), d.getVar('MACHINE')))
}
