"""ALP SDK west extension commands.

Registered subcommands (each subclasses `west.commands.WestCommand`
in its own file):

- alp_build  — pre-flight board.yaml validation then delegate to west build
- alp_clean  — clean per-slice build output trees
- alp_flash  — dispatch helper-MCU flash flows + main-image flash
- alp_image  — bundle per-slice outputs into a single deployable image
- alp_renode — boot the system manifest in Renode (dual-OS smoke test)

Each command is auto-discovered by west via the `west-commands:`
entry in `zephyr/module.yml`.
"""
