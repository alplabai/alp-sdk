#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Binary ABI gate: `abidiff` (libabigail) on the built libalp_sdk.so (#2840).

Since #2839 the soname carries the minor while the major is 0
(libalp_sdk.so.0.17), so a patch release inside one minor line must stay
ABI-compatible -- that is what makes an in-place package upgrade on the
dev-kit feed safe (#2827).  `pr-abi-snapshot` only diffs the HEADERS, and
only on main/release/**; this gate diffs the compiled library.

What it does:
  1. Picks the baseline: the highest release tag vX.Y.Z of the same
     MAJOR.MINOR as metadata/sdk_version.yaml, else that minor's highest
     rc tag.  Before the first tag of a new minor (dev right after a minor
     bump) it falls back to the PREVIOUS minor's highest release/rc tag in
     report-only mode, so dev always shows a diff instead of silently
     checking nothing (#2851).  With no tag at all it SKIPS, or FAILS
     under --require-tool.
  2. Builds libalp_sdk.so from the baseline tag and from the working tree
     with the same host compiler and the same options (the yocto
     plain-CMake shared configuration, RelWithDebInfo so DWARF is present).
  3. `abidiff --no-added-syms` with --headers-dir1/2 = each tree's
     include/ (public surface only).  Added symbols are compatible and
     are not reported.
  4. Fails when abidiff reports ABI_CHANGE (4) or ABI_INCOMPATIBLE_CHANGE
     (8) and the baseline is the same minor as the head.  Against a
     different-minor baseline (--baseline-tag; the SONAME differs) the
     diff is reported and the gate passes.

[ABI-EXPERIMENTAL] is deliberately NOT suppressed: it is a header-comment
tag that no tool extracts per symbol, and its "may change in any minor"
latitude is a minor-to-minor allowance; this gate only compares within a
minor, where nothing may break.

The `abidiff` job in pr-plain-cmake.yml has a merge_group trigger so it can
be made a required context; it blocks a merge only once the maintainer adds
that context to branch protection (#2851).  Every skip or report-only
fallback emits a ::warning:: under --require-tool or in CI (GITHUB_ACTIONS)
so it is visible on the run page.

A missing abidiff SKIPS loudly (exit 0, banner on stderr) unless
--require-tool is given (CI), in which case it fails.

Waivers (#2851): docs/abi/abidiff-waivers.yaml lists reviewed, time-boxed
exceptions (`symbol`, `kind`, `reason`, `issue`, `until`; format in that
file's header).  A change whose abidiff entry is covered by a valid waiver
is dropped and the applied waivers are printed; anything else still fails.
A malformed waiver, a missing reason/issue, or one whose `until` minor has
been reached by metadata/sdk_version.yaml FAILS the gate.  The gate as a
whole stays advisory in CI (not a required context).

Usage:
    python3 scripts/check_abidiff.py [--require-tool] [--baseline-tag TAG]
"""
from __future__ import annotations

import argparse
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

ABI_ERROR, ABI_USAGE, ABI_CHANGE, ABI_INCOMPATIBLE = 1, 2, 4, 8

WAIVER_FILE = Path("docs") / "abi" / "abidiff-waivers.yaml"
WAIVER_KINDS = ("function", "variable", "type")

BUILD_ARGS = [
    "-DALP_OS=yocto",
    "-DALP_SDK_BUILD_SHARED=ON",
    "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
]


def warn_skip(msg: str, always: bool = False) -> None:
    """GitHub ::warning:: for a skipped check (CI or --require-tool)."""
    if always or os.environ.get("GITHUB_ACTIONS"):
        print(f"::warning title=abidiff skipped::{msg}")


def sdk_minor(root: Path) -> str:
    """'0.17' from the `version:` line of metadata/sdk_version.yaml."""
    text = (root / "metadata" / "sdk_version.yaml").read_text(encoding="utf-8")
    m = re.search(r"^version:\s*(\d+)\.(\d+)\.\d+", text, re.M)
    if not m:
        raise ValueError("metadata/sdk_version.yaml: no parsable `version:`")
    return f"{m[1]}.{m[2]}"


def pick_baseline(minor: str, tags: list[str]) -> str | None:
    """Highest release tag of `minor`, else its highest rc tag, else None."""
    pre = re.escape(minor)
    rel = [(int(m[1]),) for t in tags if (m := re.fullmatch(rf"v{pre}\.(\d+)", t))]
    if rel:
        return f"v{minor}.{max(rel)[0]}"
    rcs = [
        (int(m[1]), int(m[2]), t)
        for t in tags
        if (m := re.fullmatch(rf"v{pre}\.(\d+)-rc(\d+)", t))
    ]
    return max(rcs)[2] if rcs else None


def pick_previous_minor_baseline(minor: str, tags: list[str]) -> str | None:
    """Baseline from the highest earlier minor that has a release or rc tag.

    Used only when `minor` has no tag yet.  Compared numerically
    (0.9 < 0.10); feed-* and other non-release tags never match.
    """
    cur = tuple(int(x) for x in minor.split("."))
    minors = {
        (int(m[1]), int(m[2]))
        for t in tags
        if (m := re.fullmatch(r"v(\d+)\.(\d+)\.\d+(?:-rc\d+)?", t))
    }
    for lo in sorted((m for m in minors if m < cur), reverse=True):
        base = pick_baseline(f"{lo[0]}.{lo[1]}", tags)
        if base:
            return base
    return None


def evaluate(rc: int, same_minor: bool) -> tuple[bool, str]:
    """(failed, reason) for an abidiff exit status."""
    if rc & (ABI_ERROR | ABI_USAGE):
        return True, f"abidiff itself failed (exit {rc})"
    changed = bool(rc & (ABI_CHANGE | ABI_INCOMPATIBLE))
    if not changed:
        return False, "no ABI change"
    if not same_minor:
        return False, "ABI changed but SONAME differs (new minor): report only"
    return True, (
        "incompatible ABI change inside one minor line: a patch release must "
        "stay ABI-compatible (in-place package upgrades depend on it). Revert "
        "the change, or make it in a new minor (which bumps the SONAME)"
    )


def load_waivers(path: Path, minor: str) -> tuple[list[dict], list[str]]:
    """(valid waivers, problems).  Any problem fails the gate."""
    if not path.is_file():
        return [], []
    import yaml  # only needed once a waiver file exists

    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8"))
    except yaml.YAMLError as e:
        return [], [f"{path.name}: unparsable YAML: {e}"]
    if data is None:
        data = []
    if not isinstance(data, list):
        return [], [f"{path.name}: top level must be a list of waivers"]
    cur = tuple(int(x) for x in minor.split("."))
    ok, bad = [], []
    for i, w in enumerate(data):
        tag = f"{path.name} entry {i + 1}"
        if not isinstance(w, dict):
            bad.append(f"{tag}: not a mapping")
            continue
        tag += f" ({w.get('symbol')!r})"
        errs = []
        extra = set(w) - {"symbol", "kind", "reason", "issue", "until"}
        if extra:
            errs.append(f"unknown keys {sorted(extra)}")
        for k in ("symbol", "reason"):
            if not (isinstance(w.get(k), str) and w[k].strip()):
                errs.append(f"`{k}` missing or empty")
        if w.get("kind") not in WAIVER_KINDS:
            errs.append(f"`kind` must be one of {'|'.join(WAIVER_KINDS)}")
        if not (isinstance(w.get("issue"), str) and re.fullmatch(r"#\d+", w["issue"])):
            errs.append("`issue` must be a quoted string like \"#2851\"")
        u = w.get("until")
        if not (isinstance(u, str) and re.fullmatch(r"\d+\.\d+", u)):
            errs.append("`until` must be a quoted minor string like \"0.19\"")
        elif tuple(int(x) for x in u.split(".")) <= cur:
            errs.append(f"expired: `until` {u} reached (SDK is {minor}); fix the ABI "
                        "break's follow-up or remove the waiver")
        if errs:
            bad.append(f"{tag}: " + "; ".join(errs))
        else:
            ok.append(w)
    return ok, bad


def parse_entries(report: str) -> list[dict]:
    """Top-level `[D]`/`[C]` entries of an abidiff report.

    Each: kind (function|variable), name, indirect (the header says every
    change is an indirect sub-type change), types (struct/union/enum names
    the entry's block mentions).  Typedef/anonymous types and array
    variables are not recognised, so they can never be waived.
    """
    entries: list[dict] = []
    for ln in report.splitlines():
        m = re.match(r"  \[[DC]\] '(.*?)'(.*)", ln)
        if m:
            decl, rest = m[1], m[2]
            if decl.startswith("function "):
                kind, nm = "function", re.search(r"(\w+)\(", decl)
            else:
                kind, nm = "variable", re.search(r"(\w+)$", decl)
            sym = re.search(r"\{(\w+)\}", rest)
            entries.append({"kind": kind,
                            "indirect": "has some indirect sub-type changes" in rest,
                            "name": sym[1] if sym else (nm[1] if nm else decl),
                            "types": set()})
        elif entries and ln.startswith("   "):
            entries[-1]["types"].update(re.findall(r"'((?:struct|union|enum) \w+)'", ln))
    return entries


def apply_waivers(report: str, waivers: list[dict]) -> tuple[bool, list[dict]]:
    """(everything covered, waivers that matched an entry).

    Covered only when every Removed/Changed in the summaries was parsed into
    an entry and each entry is covered; otherwise the caller keeps abidiff's
    rc.  A `type` waiver covers an entry only when ALL its reported changes
    are indirect sub-type changes and every struct/union/enum it mentions is
    waived; a function that also gained a parameter stays un-waived.
    """
    entries = parse_entries(report)
    expected = sum(
        int(n) for ln in re.findall(r"changes summary: (.*)", report)
        for n in re.findall(r"(\d+) (?:Removed|Changed)", ln)
    )
    if not entries or len(entries) != expected:
        return False, []
    twaived = {w["symbol"] for w in waivers if w["kind"] == "type"}
    used: list[dict] = []
    covered = True
    for e in entries:
        hit = [w for w in waivers if w["kind"] == e["kind"] and w["symbol"] == e["name"]]
        if e["indirect"] and e["types"] and e["types"] <= twaived:
            hit += [w for w in waivers if w["kind"] == "type" and w["symbol"] in e["types"]]
        covered &= bool(hit)
        used += [w for w in hit if w not in used]
    return covered, used


def soname_only(report: str) -> bool:
    """True when abidiff's only finding is the ELF SONAME rename.

    A baseline built before #2839 carries SONAME libalp_sdk.so.0; comparing
    it to a same-minor head always sets ABI_CHANGE for that rename alone.
    Every "changes summary" line being all-zero means nothing else moved.
    """
    lines = re.findall(r"changes summary: (.*)", report)
    nums = [n for ln in lines for n in re.findall(r"(\d+) (?:Removed|Changed|Added)", ln)]
    return len(lines) >= 2 and all(n == "0" for n in nums) and "SONAME changed" in report


def soname_of(lib: Path) -> str | None:
    if not shutil.which("readelf"):
        return None
    out = subprocess.run(
        ["readelf", "-d", str(lib)], capture_output=True, text=True, encoding="utf-8", check=False
    ).stdout
    m = re.search(r"SONAME.*\[(.+)\]", out)
    return m[1] if m else None


def build_lib(src: Path, build: Path) -> Path:
    cfg = ["cmake", "-S", str(src), "-B", str(build), *BUILD_ARGS]
    for gen, tool in (("Ninja", "ninja"), ("Unix Makefiles", "make")):
        if shutil.which(tool):
            cfg += ["-G", gen]
            break
    subprocess.run(cfg, check=True)
    subprocess.run(["cmake", "--build", str(build), "--parallel"], check=True)
    libs = sorted(build.rglob("libalp_sdk.so"))
    if not libs:
        raise FileNotFoundError(f"no libalp_sdk.so under {build}")
    return libs[0]


def export_tag(root: Path, tag: str, dest: Path) -> None:
    tar = subprocess.run(
        ["git", "-C", str(root), "archive", "--format=tar", tag],
        capture_output=True, check=True,
    ).stdout
    with tarfile.open(fileobj=io.BytesIO(tar)) as tf:
        tf.extractall(dest)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--root", type=Path, default=REPO)
    ap.add_argument("--baseline-tag", help="override the auto-picked baseline")
    ap.add_argument("--require-tool", action="store_true",
                    help="missing abidiff/cmake fails instead of skipping (CI)")
    ap.add_argument("--keep", action="store_true", help="keep the temp build dirs")
    args = ap.parse_args()

    root = args.root
    minor = sdk_minor(root)
    waivers, problems = load_waivers(root / WAIVER_FILE, minor)
    if problems:
        for p in problems:
            print(f"FAIL: {p}", file=sys.stderr)
        return 1
    for tool in ("abidiff", "cmake"):
        if not shutil.which(tool):
            msg = f"{tool} not found"
            if args.require_tool:
                warn_skip(f"{msg}; nothing was checked", True)
                print(f"FAIL: {msg} (--require-tool)", file=sys.stderr)
                return 1
            warn_skip(f"{msg}; nothing was checked")
            print("=" * 70, file=sys.stderr)
            print(f"SKIPPED check_abidiff: {msg}. NOTHING WAS CHECKED.", file=sys.stderr)
            print("Install libabigail (apt install abigail-tools) to run this gate.",
                  file=sys.stderr)
            print("=" * 70, file=sys.stderr)
            return 0

    tags = subprocess.run(["git", "-C", str(root), "tag", "--list", "v*"],
                          capture_output=True, text=True, encoding="utf-8", check=True).stdout.split()
    tag = args.baseline_tag or pick_baseline(minor, tags)
    if tag is None:
        tag = pick_previous_minor_baseline(minor, tags)
        if tag is None:
            warn_skip(f"no release tag at all (minor {minor}): nothing compared",
                      args.require_tool)
            if args.require_tool:
                print(f"FAIL: no v* release tag to compare against (--require-tool); "
                      f"in CI make sure tags are fetched.", file=sys.stderr)
                return 1
            print("SKIP: no release tag to compare against.")
            return 0
        warn_skip(f"no v{minor}.* tag yet: comparing against {tag} (previous minor), "
                  f"report-only; the blocking check starts at the first tag of minor {minor}",
                  args.require_tool)
        print(f"note: no v{minor}.* tag yet; {tag} is a report-only baseline.")

    work = Path(tempfile.mkdtemp(prefix="abidiff-"))
    try:
        base_src = work / "base-src"
        base_src.mkdir()
        export_tag(root, tag, base_src)
        base_lib = build_lib(base_src, work / "base-build")
        head_lib = build_lib(root, work / "head-build")
        s1, s2 = soname_of(base_lib), soname_of(head_lib)
        print(f"baseline {tag}: SONAME {s1}; head: SONAME {s2}")
        proc = subprocess.run(
            ["abidiff", "--no-added-syms",
             "--headers-dir1", str(base_src / "include"),
             "--headers-dir2", str(root / "include"),
             str(base_lib), str(head_lib)],
            capture_output=True, text=True, encoding="utf-8", check=False,
        )
        print(proc.stdout, end="")
        print(proc.stderr, end="", file=sys.stderr)
        same_minor = tag.startswith(f"v{minor}.")
        rc = proc.returncode
        if not (rc & (ABI_ERROR | ABI_USAGE)) and same_minor and s1 != s2 and soname_only(proc.stdout):
            print("note: only the SONAME rename differs (pre-#2839 baseline); ignored")
            rc = 0
        if rc & (ABI_CHANGE | ABI_INCOMPATIBLE) and not rc & (ABI_ERROR | ABI_USAGE) and waivers:
            covered, used = apply_waivers(proc.stdout, waivers)
            for w in used:
                print(f"waived: {w['kind']} {w['symbol']} ({w['issue']}, until "
                      f"{w['until']}): {w['reason']}")
            for w in (w for w in waivers if w not in used):
                print(f"warning: waiver {w['kind']} {w['symbol']} ({w['issue']}) "
                      "matched nothing; remove it if stale")
            # A SONAME line is only tolerated like the pre-#2839 case above.
            soname_ok = "SONAME changed" not in proc.stdout or (same_minor and s1 != s2)
            if covered and soname_ok:
                rc = 0
        # same_minor is false only with a --baseline-tag override.
        failed, why = evaluate(rc, same_minor)
        print(("FAIL: " if failed else "OK: ") + why)
        return 1 if failed else 0
    finally:
        if args.keep:
            print(f"kept {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
