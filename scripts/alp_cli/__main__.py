"""`python -m alp_cli` -- the ONLY way to run this package.

**This is not a front door, and there is no `alp` command.** `pyproject.toml`
deliberately installs no `alp` console-script, and ADR 0020 (§124-126) states
the rule in the same words: `alp_cli` is tan's Python backend, "invoked as
`python -m alp_cli <sub>` -- never as a user-installed `alp` binary". The
user-facing command surface is `tan` (`alplabai/tan-cli`).

This docstring used to claim parity with "the installed `alp` console script".
That script has never existed, so the claim sent a reader looking for a binary
no packaging step produces (alp-sdk#1193). `prog_name` follows: usage and error
lines now name `python -m alp_cli`, which is a command someone can actually
run, instead of `alp`, which is not.

Several `alp_cli` verbs share a NAME with a `tan` verb while meaning something
different -- `generate` here materialises a template into a directory, while
`tan generate` emits board-derived artefacts. Do not treat the two surfaces as
interchangeable, and do not add a console entry point without the parity work
alp-sdk#1193 requires first.
"""

from __future__ import annotations

from alp_cli.main import cli

if __name__ == "__main__":
    cli(prog_name="python -m alp_cli")
