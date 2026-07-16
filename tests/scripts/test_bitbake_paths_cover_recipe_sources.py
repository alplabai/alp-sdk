# SPDX-License-Identifier: Apache-2.0
"""`pr-bitbake.yml` must trigger on the example source its recipes compile.

The class gate for a bug that shipped silently: #818 fixed
`alp-lvgl-dashboard`'s `do_compile`, and merging it triggered NO bake --
`examples/display/lvgl-dashboard-x-evk/CMakeLists.txt`, the very file the
recipe compiles, matched none of the workflow's `paths:` filters.  Only
`examples/**/board.yaml` did.  So a Yocto-breaking edit to example source
could land on `dev` with the Yocto CI silently not running.

The filters are deliberately NOT a blanket `examples/**`: a bake is hours
long and runs on the bench host, so a Zephyr-only example edit must not
trigger one.  That precision is the reason this test has to exist -- an
explicit list rots the moment someone adds a recipe and forgets the
workflow, and the failure mode is silence, not red.

The oracle is the RECIPES, never a hand-typed list: every
`S = ${WORKDIR}/git/examples/<...>` in `meta-alp-sdk/**.bb` must be matched
by some `paths:` entry in both the `pull_request` and `push` filters.
"""

from __future__ import annotations

import fnmatch
import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github" / "workflows" / "pr-bitbake.yml"
RECIPES = REPO / "meta-alp-sdk"

# `S = "${WORKDIR}/git/examples/display/lvgl-dashboard-x-evk"` -> the examples/ path
_S_EXAMPLES = re.compile(r'^\s*S\s*=\s*"[^"]*?(examples/[^"]+)"', re.M)


def _recipe_example_sources() -> dict[str, Path]:
    """Map examples/<...> source dir -> the .bb that builds it."""
    found: dict[str, Path] = {}
    for bb in sorted(RECIPES.rglob("*.bb")):
        for m in _S_EXAMPLES.finditer(bb.read_text(encoding="utf-8")):
            found[m.group(1).rstrip("/")] = bb
    return found


def _paths_for(event: str) -> list[str]:
    """The `paths:` globs under a given `on:` event in pr-bitbake.yml.

    Hand-parsed rather than via PyYAML: the file's comments carry the WHY
    for each entry and a round-trip through a plain loader would encourage
    someone to rewrite it and drop them.
    """
    text = WORKFLOW.read_text(encoding="utf-8")
    m = re.search(rf"^  {event}:$", text, re.M)
    assert m, f"no `{event}:` trigger in {WORKFLOW}"
    rest = text[m.end():]
    end = re.search(r"^  \w[\w-]*:", rest, re.M)
    block = rest[: end.start()] if end else rest
    pm = re.search(r"^    paths:$", block, re.M)
    if not pm:
        return []
    tail = block[pm.end():]
    stop = re.search(r"^    \w", tail, re.M)
    tail = tail[: stop.start()] if stop else tail
    return re.findall(r"^\s*-\s*'([^']+)'", tail, re.M)


def _covered(source_dir: str, globs: list[str]) -> bool:
    """Would an edit to a file under `source_dir` match any glob?"""
    probe = f"{source_dir}/src/main.c"
    for g in globs:
        # GitHub's `**` spans directories; fnmatch's `*` already does, so a
        # plain translate is close enough for the shapes used here.
        if fnmatch.fnmatch(probe, g) or fnmatch.fnmatch(probe, g.replace("/**", "/*")):
            return True
    return False


def test_recipes_build_some_example_source() -> None:
    """Guard the guard: if this finds nothing, the regex broke, and every
    assertion below would vacuously pass."""
    assert _recipe_example_sources(), (
        "no `S = .../examples/...` found in meta-alp-sdk/**.bb -- the regex "
        "is stale, so the coverage test below would pass vacuously"
    )


@pytest.mark.parametrize("event", ["pull_request", "push"])
def test_bitbake_paths_cover_every_recipe_example_source(event: str) -> None:
    globs = _paths_for(event)
    assert globs, f"`{event}` has no paths: filter -- it would bake on every change"
    missing = {
        src: bb.relative_to(REPO)
        for src, bb in _recipe_example_sources().items()
        if not _covered(src, globs)
    }
    assert not missing, (
        "pr-bitbake.yml `on.{ev}.paths` does not cover example source that "
        "meta-alp-sdk recipes compile, so a Yocto-breaking edit there lands "
        "with NO bake (exactly how #818's do_compile fix merged unbuilt):\n"
        + "\n".join(f"  {src}  (built by {bb})" for src, bb in sorted(missing.items()))
        + "\n\nAdd '<dir>/**' to the paths: list for BOTH pull_request and push."
    ).format(ev=event)


def test_pull_request_and_push_paths_agree() -> None:
    """The two lists must stay in lockstep -- a PR that bakes and a merge
    that doesn't (or vice versa) is worse than either alone."""
    pr, push = set(_paths_for("pull_request")), set(_paths_for("push"))
    assert pr == push, (
        "pr-bitbake.yml pull_request/push paths: diverged --\n"
        f"  only in pull_request: {sorted(pr - push)}\n"
        f"  only in push:         {sorted(push - pr)}"
    )
