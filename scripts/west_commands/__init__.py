"""Alp SDK west extension commands.

Registered subcommands (each subclasses `west.commands.WestCommand`
in its own file):

- alp-lock    — write or verify the project's alp.lock dependency lock
- alp-migrate — version and migrate a project's board.yaml
- alp-quality — run the quality-task registry for a profile (JSON/JUnit/SARIF)
- alp-emit    — print a generated config artefact from board.yaml (no build)

Each command is auto-discovered by west via the `west-commands:`
entry in `zephyr/module.yml` (see scripts/west-commands.yml, the
authoritative registration list).
"""
