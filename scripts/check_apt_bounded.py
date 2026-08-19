#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Reject a raw `apt-get update`/`apt-get install` in a workflow step.

`Acquire::http::Timeout` bounds an IDLE read, not a SLOW one: every byte
that arrives resets the timer, and apt has no minimum-transfer-rate
option, so a mirror that trickles a byte every few seconds defeats it
forever. Measured against two local servers via `apt-helper download-file`
(same Acquire stack as apt-get, issue #1575):

  server                              result
  -----------------------------------  ------------------------------------
  accepts, sends headers, then silent  rc=100 after 127s -- Timeout=30
                                        FIRED, 3 retries, apt gave up itself
  accepts, then 1 byte every 20s       NEVER returns; only an external kill
                                        ended it. Unbounded.

scripts/ci/apt-bounded.sh (#1575) adds the only thing that bounds the
trickle class: a wall-clock `timeout` per attempt, a `dpkg --configure -a`
recovery before each retry, and a retry only on rc 124/100. A future
workflow step that calls `apt-get update`/`apt-get install` directly
reintroduces the unbounded hang this issue fixed -- this gate catches that
at review time instead of an 11-hour CI outage (the trickle case had been
failing `Install host build tools` / `Install cppcheck` / `Install
Doxygen` / `Install arm-none-eabi toolchain` for that long before #1575).

Scope: only a line that is ITSELF an apt-get invocation (optionally
`sudo`-prefixed), i.e. `^\\s*(sudo )?apt-get (update|install)\\b`. This
deliberately does not match:
  - a quoted fixture asserting doc/hint text
    (`"sudo apt-get install -y cmake"`, onramp-clean-container.yml),
  - a generated-manifest string literal (`print("apt-get update -qq")`,
    pr-bootstrap-distro-install.yml),
  - a single-line `run: apt-get install ...` step with no
    `-o Acquire::*` flags (a fast, already-installed-typical tool fetch,
    not the class #1575 measured),
because none of those lines start with `apt-get`/`sudo apt-get` after
leading whitespace.

Allowlist: a line matching the pattern is still permitted if it carries a
trailing `# apt-bounded:allow (...)` comment. Used exactly once today, in
onramp-clean-container.yml's `prereqs-and-bootstrap-tan` job: the apt-get
call there installs git/curl/ca-certificates so `actions/checkout` can run
AT ALL on a bare `ubuntu:24.04` image -- the wrapper script isn't on disk
yet at that point in the job, so it cannot be called. A growing allowlist
here would mean the gate is being defeated rather than genuinely
inapplicable -- keep it to that one structurally-forced case.

Run locally:

    python3 scripts/check_apt_bounded.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

_APT_RAW_RE = re.compile(r"^\s*(sudo )?apt-get (update|install)\b")
_ALLOW_MARKER = "# apt-bounded:allow"


def find_problems(root: Path) -> list[str]:
    problems: list[str] = []
    workflows_dir = root / ".github" / "workflows"
    if not workflows_dir.is_dir():
        return problems

    for wf_path in sorted(workflows_dir.glob("*.yml")):
        rel = wf_path.relative_to(root)
        for lineno, line in enumerate(wf_path.read_text(encoding="utf-8").splitlines(), start=1):
            if not _APT_RAW_RE.match(line):
                continue
            if _ALLOW_MARKER in line:
                continue
            problems.append(
                f"{rel}:{lineno}: raw {line.strip()!r} -- Acquire::http::Timeout "
                f"bounds an idle read, not a trickling one (#1575); call "
                f"scripts/ci/apt-bounded.sh update / install ... instead, or add "
                f"a trailing '{_ALLOW_MARKER} (reason)' comment if the wrapper "
                f"genuinely isn't reachable yet (e.g. before checkout)."
            )
    return problems


def main() -> int:
    problems = find_problems(ROOT)
    if problems:
        print("check_apt_bounded: raw apt-get call(s) that bypass scripts/ci/apt-bounded.sh:",
              file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        print(f"\ncheck_apt_bounded: {len(problems)} problem(s) -- failing.", file=sys.stderr)
        return 1
    print("check_apt_bounded: OK (every apt-get update/install in "
          ".github/workflows/*.yml goes through scripts/ci/apt-bounded.sh, "
          "or is explicitly allowlisted).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
