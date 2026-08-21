#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Keep metadata/toolchains.json (the Zephyr SDK pin) in lockstep with
everything that copies it.

metadata/toolchains.json (issue #949 item 3) is the single source of truth
for the pinned Zephyr SDK release: its version, and the per-artifact upstream
URL + sha256 + size. Without a drift gate it is just a copy of facts that
already lived (inconsistently, unverified) in four CI workflows. This check
fails loudly when:

  1. metadata/toolchains.json doesn't validate against
     metadata/schemas/toolchains-v1.schema.json.
  2. `zephyrSdk.version` disagrees with the real `SDK_VERSION` file at the
     Zephyr revision metadata/bootstrap.json's `zephyr.version` pins --
     read from the pinned Zephyr checkout with `git show <rev>:SDK_VERSION`
     (never the checkout's *currently checked out* ref: a local dev clone is
     routinely a patch behind/ahead of the pin, and `git show <rev>:<path>`
     reads the blob at `<rev>` straight out of the object store regardless
     of what HEAD currently points at, so a stale local checkout doesn't
     need to be re-checked-out just to serve as the oracle). See
     `_check_sdk_version_matches_zephyr_pin`'s own docstring for the
     skip-vs-fail policy when no such checkout resolves at all (CI runners
     and a bare `pip install` both routinely have none).
  3. ANY `.github/workflows/*.yml` file -- not a curated list -- names the
     sdk-ng release URL (derived from `zephyrSdk.baseUrl`) or one of the
     manifest's own artifact filename shapes (derived from each
     `zephyrSdk.artifacts[].filename`) with a version slot that is not the
     manifest-sourced `${{ env.ZEPHYR_SDK_VERSION }}` / `${ZEPHYR_SDK_VERSION}`
     reference; or names either one with no `ZEPHYR_SDK_SHA256` reference
     anywhere in the file (a download with no integrity check); or has a
     cache `key:` line naming an SDK cache with a literal Zephyr-shaped
     `vX.Y.Z` baked in instead of `${{ env.ZEPHYR_SDK_VERSION }}` -- the
     category confusion (naming a *Zephyr* version on an *SDK* cache) that
     let pr-twister.yml silently reuse a stale cached toolchain forever
     across an SDK bump. See `_check_sdk_reference_drift`'s own docstring
     for the full, authoritative list of what it scans for.
  4. Any of TOOLCHAIN_WORKFLOWS below -- today TWO files, the survivors of
     the four workflows issue #949 item 3 originally named (the other two
     were pr-renode-aen-smoke.yml and pr-renode-sim-mode.yml, deleted by
     docs/adr/0022 Amendment 2's Renode retirement) -- no longer contains
     a real read of metadata/toolchains.json at all. This is a DIFFERENT
     assertion from check 3 above: check 3 scans every workflow,
     repo-wide, for a drifted COPY of a manifest fact; this one polices
     those specific files for still HAVING a manifest read in the first
     place -- so silently deleting just that one step (while every literal
     elsewhere in the file, e.g. an unchanged `${ZEPHYR_SDK_URL}`
     reference, still looks untouched) is caught too. See
     `_check_still_reads_manifest`.
  5. A `zephyrSdk.artifacts[]` row's `tier` or `licence` (issue #1603,
     follow-on to #1574) disagrees with a sibling row for the SAME
     `component` (e.g. `minimal-sdk` on linux-x86_64 says tier A but the
     windows-x86_64 row for the same component says tier B) -- `tier`/
     `licence` are per-row fields but `component` is the real artifact
     identity, repeated once per `host`. Presence of both fields is
     enforced by check 1 (the schema's `required` list); this is the
     cross-row CONSISTENCY check schema validation cannot express. See
     `_check_artifact_provenance_consistency`.

Why check 3 looks like this (history): this gate originally scanned only
TOOLCHAIN_WORKFLOWS -- then a curated, four-file list, with the stated
rationale that "a repo-wide scan over every *.yml would need its own
allowlist for false positives". That is the exact argument already made,
and rejected, for the sibling `check_bootstrap_manifest.py` gate's own
winget-identifier scan (issue #949 review): a verb-triggered repo-wide scan
there needed a sixteen-entry allowlist of legitimate lines, and re-keying
the trigger on a DISTINCTIVE IDENTIFIER derived from the manifest (the
winget package ID, not a bare "winget install" verb) collapsed that
allowlist to zero. Applied here:
the distinctive identifiers are the sdk-ng release URL path
(`zephyrSdk.baseUrl`, minus its trailing version path segment) and the
artifact filename shapes (each `zephyrSdk.artifacts[].filename`, minus its
version substring) -- both derived from metadata/toolchains.json AT RUN
TIME by `_sdk_ng_url_trigger`/`_artifact_filename_triggers`, never hardcoded
a second time in this script. A prose comment quoting a bare version number
contains neither, so the false-positive class the old rationale feared does
not actually occur (see the repo-wide sweep this change ran: today, ZERO
non-curated workflow mentions sdk-ng/zephyr-sdk/ZEPHYR_SDK at all).

The curated four-file scan-scope this replaces was also a real, reproduced
bypass, not a hypothetical: it protected exactly the four files it named and
nothing past them, so a FIFTH workflow dropping in its own SDK install with
a hardcoded version, a hardcoded URL, and no hash check at all passed this
gate cleanly -- reproducing precisely the four defects it exists to prevent.
Widening the OLD name-keyed regexes (`SDK_VER=`/`ZEPHYR_SDK_VERSION=`, and a
literal digit in the sdk-ng URL) to every file would not have been enough
either: a version supplied only as some OTHER env var (`SDKV: 0.16.8`, say)
with the URL built from `${SDKV}` has no digit following a `=`/`:` for that
regex to key on, and no literal digit in the URL for the old URL regex to
key on. `_sdk_ng_url_trigger`/`_artifact_filename_triggers` fire on the URL
or filename SHAPE regardless of what fills the version slot -- a literal
digit sequence, or a wrongly-named variable -- and then check ONLY that the
slot is exactly the canonical manifest-sourced reference; no digit is
required in the trigger itself, which is what makes the "wrong variable
name" bypass unable to hide the way it could from a name-keyed regex.

KNOWN, DELIBERATE LIMIT: a `container: <image>:<tag>` pin (e.g.
`zephyrprojectrtos/ci:v0.26.13`) is NOT scanned by check 3. A container tag
bundles an entire prebuilt toolchain image, not the URL/version/hash triple
checks 3 and 4 police -- there is no "download this, verify this hash" shape
in a bare image reference for either check to evaluate. pr-twister.yml's own
header comment already explains why this repo's CI does not run off that
image (disk space, not an integrity concern this gate could catch anyway).
Recording the limit here, rather than silently leaving it unscanned, is
deliberate: a gate that states what it does not cover beats one that implies
coverage it does not have.

This gate deliberately does NOT check `scripts/check_bootstrap_manifest.py`'s
own territory (the *Zephyr* revision pin) -- that script's module docstring
already scopes itself OUT of "the separate Zephyr SDK toolchain cache, which
tracks its own release" (see its point 4). This script is what fills that
deliberately-left gap; it does not duplicate `check_bootstrap_manifest.py`'s
Zephyr-version checks, only reads `zephyr.version` as the input to point 2
above.

Git-blame finding on the historical `SDK_VER=0.16.8` in
pr-renode-aen-smoke.yml (the workflow itself no longer exists -- deleted by
docs/adr/0022 Amendment 2; read the finding off git history): introduced in
commit cbb8a44... adding the workflow
from scratch, at a time this repo already pinned Zephyr v4.4.0 (whose own
`SDK_VERSION` was already `1.0.1`, same as at the current v4.4.1 pin) -- so
`0.16.8` was never correct for any Zephyr revision this repo has built
against; it is a plain wrong value, not a value that drifted true-to-false
over time. `0.16.8` was itself a real, historical Zephyr SDK release
(superseded by 0.16.9, then 0.17.0, then eventually 1.0.1 in Zephyr's own
history) -- most likely copied from an older tutorial/example without
checking it against the Zephyr version this repo actually pins.

Run locally:

    python3 scripts/check_toolchain_lock.py

Set ALP_REQUIRE_ZEPHYR_ORACLE=1 in a job that promises a pinned Zephyr
checkout is present (mirrors the same flag `tests/scripts/
test_hil_blocks_coverage.py`'s `#807` class gate already uses for the
identical "workspace not resolvable" situation) to turn check 2's skip into
a hard failure instead.

SCOPE, and what it does NOT cover (issue #1012). Check 3 scans every
`.github/workflows/*.yml` in THIS repo. The invariant people read off that
sentence is broader than what it enforces: the real one is "nothing outside
metadata/toolchains.json hardcodes the SDK version", and this gate can only
police one repo's workflows.

The live counter-example is a different repo. `alplabai/tan-cli` consumes the
same fact -- `tan doctor`'s `zephyrSdk` check names the exact
`west sdk install --version <..>` remedy a customer runs -- from
`ZEPHYR_SDK_INSTALL_VERSION` in `python/tan/commands/doctor_cmd.py`, which has
to track `zephyrSdk.version` here
by hand. This script cannot see that checkout, so a bump here could leave tan
printing a stale install command with nothing red on either side. tan-cli
issue #172, closed by its PR #173, covers that half from the other end: it
vendors `metadata/toolchains.json` into `contract/fixtures/toolchains/` and
asserts byte-parity against the constant in Tan's required Python tests --
the same shape its bootstrap-manifest fallback already uses. So the pair is
covered TODAY, by a gate on the other side of the seam, not by this one.

Stated here because the wording is what let the gap open: a scope sentence
that reads like a guarantee invites exactly the copy it does not cover. When a
NEW consumer of this pin appears, in this repo or another, it needs its own
parity assertion -- widening this scan will not reach it. Same caveat applies
to `metadata/toolchains.json`'s own `_comment`, which repeats the CI-workflow
scoping verbatim in its point (3).
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

import jsonschema

REPO = Path(__file__).resolve().parent.parent
MANIFEST = REPO / "metadata" / "toolchains.json"
SCHEMA = REPO / "metadata" / "schemas" / "toolchains-v1.schema.json"
BOOTSTRAP_MANIFEST = REPO / "metadata" / "bootstrap.json"
WORKFLOWS_DIR = REPO / ".github" / "workflows"

# The CI workflows issue #949 item 3 originally named as "the only things
# that reference the Zephyr SDK" -- kept here, but demoted from "the scan's
# full scope" to a POSITIVE assertion (check 4, module docstring): each of
# these must still contain a real read of metadata/toolchains.json
# somewhere. Check 3's repo-wide scan below is what now defines the actual
# scan scope (`_iter_workflow_files`); this list's only remaining job is to
# catch one of these having its manifest-read step silently deleted, which a
# repo-wide COPY-drift scan cannot see for a file (like pr-twister.yml) that
# names no sdk-ng URL/filename literal to begin with.
#
# #949 named FOUR. Two of them -- pr-renode-aen-smoke.yml and
# pr-renode-sim-mode.yml -- were deleted by docs/adr/0022 Amendment 2's
# Renode retirement, so the list is two today. Shrinking it is not a
# weakening of check 4: check 4 only ever asserted a property OF the files
# it names, and a deleted file has no manifest-read step to lose. Nothing
# else changed about which files are eligible; a future workflow that grows
# a manifest read belongs here too.
TOOLCHAIN_WORKFLOWS = [
    REPO / ".github" / "workflows" / "pr-getting-started-aen801.yml",
    REPO / ".github" / "workflows" / "pr-twister.yml",
]

# A real read of the manifest -- `open("metadata/toolchains.json")` or
# `open("alp-sdk/metadata/toolchains.json")` (the `path:`-checked-out form
# one of the two TOOLCHAIN_WORKFLOWS uses -- pr-getting-started-aen801.yml;
# pr-twister.yml uses the plain form) -- as opposed to merely
# MENTIONING the path in a comment, which a stale rationale comment left
# behind after the real read was deleted would still do. Used by both
# `_check_sdk_reference_drift`'s per-file "did this actually read the
# manifest" test and `_check_still_reads_manifest` below -- one regex, not
# two copies that could disagree on what "reads the manifest" means.
_MANIFEST_READ_RE = re.compile(r'open\(\s*["\'][^"\']*metadata/toolchains\.json["\']\s*\)')

# The canonical manifest-sourced reference to `zephyrSdk.version` that MUST
# fill the wildcarded version slot in an sdk-ng URL/artifact-filename
# trigger match (see `_check_sdk_reference_drift`): the GitHub Actions
# expression form, or either common shell-interpolation form. Anything
# else in that slot -- a literal digit sequence, or a differently-named
# variable -- is not sourced from the manifest.
_CANONICAL_VERSION_REF_RE = re.compile(
    r"^\$\{\{\s*env\.ZEPHYR_SDK_VERSION\s*\}\}$"
    r"|^\$\{?ZEPHYR_SDK_VERSION\}?$"
)

# A cache `key:` line naming an SDK cache with a literal Zephyr-shaped
# `vX.Y.Z` baked in -- the defect-4 category-confusion shape: a *Zephyr*
# version literal on an *SDK* cache key, instead of
# `${{ env.ZEPHYR_SDK_VERSION }}`. Independent of the trigger regexes above
# (those match a specific URL/filename shape; this one matches ANY bare
# `vX.Y.Z` on a line that also names an "sdk" cache).
_LITERAL_VERSION_RE = re.compile(r"v\d+\.\d+\.\d+")


def _load_manifest_and_schema() -> tuple[dict, list[str]]:
    problems: list[str] = []
    if not MANIFEST.is_file():
        return {}, [f"missing {MANIFEST.relative_to(REPO).as_posix()}"]
    if not SCHEMA.is_file():
        return {}, [f"missing {SCHEMA.relative_to(REPO).as_posix()}"]
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    jsonschema.Draft202012Validator.check_schema(schema)
    validator = jsonschema.Draft202012Validator(schema)
    errors = sorted(validator.iter_errors(manifest), key=lambda e: list(e.absolute_path))
    for err in errors:
        loc = "/".join(str(p) for p in err.absolute_path) or "<root>"
        problems.append(f"schema: {loc}: {err.message}")
    return manifest, problems


def _pinned_zephyr_version() -> str | None:
    if not BOOTSTRAP_MANIFEST.is_file():
        return None
    try:
        doc = json.loads(BOOTSTRAP_MANIFEST.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return (doc.get("zephyr") or {}).get("version")


def _resolve_zephyr_dir() -> Path:
    """Same resolution order as `tests/scripts/test_hil_blocks_coverage.py`'s
    `_pinned_zephyr_sysbuild_kconfig_symbols`: `$ZEPHYR_BASE` (the
    convention every `west` command and `scripts/alp_cli/doctor.py` use),
    falling back to the west-workspace topdir's conventional `zephyr/`
    project directory (`scripts/bootstrap.sh` does `west init -l <alp-sdk>`,
    so alp-sdk's parent is the topdir)."""
    env_base = os.environ.get("ZEPHYR_BASE")
    return Path(env_base) if env_base else REPO.parent / "zephyr"


def _sdk_version_in_pinned_zephyr(zephyr_dir: Path, pinned_version: str) -> str | None:
    """Read `SDK_VERSION` at `pinned_version` straight from `zephyr_dir`'s
    git object store via `git show <rev>:SDK_VERSION`, never from the
    working tree's currently-checked-out files -- a local dev clone is
    routinely a patch behind/ahead of the pin, and `git show` resolves the
    blob at the named revision regardless of what's currently checked out,
    so a stale checkout doesn't need to be re-checked-out to serve as the
    oracle. Returns None (unresolvable, never a hard error) when
    `zephyr_dir` doesn't exist, isn't a git checkout, or doesn't have
    `pinned_version` as a reachable ref (e.g. a checkout that never fetched
    that tag) -- any of these means "no oracle available", the same
    single outcome `main()` treats as skip-or-fail depending on
    ALP_REQUIRE_ZEPHYR_ORACLE."""
    if not (zephyr_dir / ".git").exists():
        return None
    try:
        result = subprocess.run(
            ["git", "-C", str(zephyr_dir), "show", f"{pinned_version}:SDK_VERSION"],
            capture_output=True, text=True, timeout=15, check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def _check_sdk_version_matches_zephyr_pin(manifest: dict) -> tuple[list[str], str | None]:
    """Check 2 (module docstring). Returns (problems, skip_reason).

    Skip-vs-fail policy: whether a pinned Zephyr checkout is resolvable at
    all is an ENVIRONMENT FACT, not a manifest defect -- a bare `pip
    install`, a pure-Python CI job, or a fresh clone with no workspace
    assembled yet all legitimately have no Zephyr checkout to read
    `SDK_VERSION` from. Hard-failing there would make this gate the kind
    people patch around or delete (the exact failure mode
    `test_hil_blocks_coverage.py`'s own `#807`-class gate already documents
    for the identical situation: it shipped once with NO job satisfying its
    precondition, so it skipped on every PR and enforced nothing silently).
    So the default is skip-with-reason, mirroring that precedent exactly --
    reusing its SAME `ALP_REQUIRE_ZEPHYR_ORACLE=1` escape hatch (rather than
    inventing a second, differently-named flag for the same concept) lets a
    job that DOES assemble a Zephyr workspace before running this gate
    (e.g. pr-twister.yml's shard 1, which already sets this flag for its own
    SB_CONFIG oracle check) turn the skip into a hard failure: there, an
    unresolvable checkout is a bug in the job's own setup, not a fact of
    the environment, and staying silently green would repeat the same
    defect.

    The alternative considered -- read-from-a-recorded-value (bake the
    derivation's result into metadata/toolchains.json itself, e.g. a
    `derivedAtZephyrRevision` field, and never re-derive at run time) -- was
    rejected: it would make the gate check that the manifest agrees with
    itself, which cannot catch the exact defect it exists for (a maintainer
    hand-bumping `zephyrSdk.version` to a wrong value with no live Zephyr
    checkout to catch it against). A live re-derivation, skipped when
    unavailable rather than always trusted, is strictly more honest about
    what it did and did not verify on a given run.
    """
    pinned_zephyr_version = _pinned_zephyr_version()
    if pinned_zephyr_version is None:
        # metadata/bootstrap.json missing/unreadable is its own gate's
        # problem (check_bootstrap_manifest.py); this gate has nothing to
        # compare against, so treat it the same as no oracle: skip.
        return [], "metadata/bootstrap.json zephyr.version not readable -- skipping SDK/Zephyr cross-check"

    zephyr_dir = _resolve_zephyr_dir()
    sdk_version_at_pin = _sdk_version_in_pinned_zephyr(zephyr_dir, pinned_zephyr_version)

    if sdk_version_at_pin is None:
        reason = (
            f"no Zephyr checkout resolved at {zephyr_dir} with "
            f"{pinned_zephyr_version} as a reachable git revision -- "
            f"skipping the metadata/toolchains.json zephyrSdk.version <-> "
            f"Zephyr SDK_VERSION cross-check"
        )
        if os.environ.get("ALP_REQUIRE_ZEPHYR_ORACLE") == "1":
            return (
                [
                    f"ALP_REQUIRE_ZEPHYR_ORACLE=1 but {reason[len('no '):]}"
                    f" -- this job promised the oracle and did not deliver "
                    f"it; fix the job's checkout, do not drop the flag"
                ],
                None,
            )
        return [], reason

    manifest_sdk_version = manifest["zephyrSdk"]["version"]
    if sdk_version_at_pin != manifest_sdk_version:
        return (
            [
                f"metadata/toolchains.json zephyrSdk.version "
                f"{manifest_sdk_version!r} disagrees with Zephyr "
                f"{pinned_zephyr_version}'s own SDK_VERSION "
                f"{sdk_version_at_pin!r} (git -C {zephyr_dir} show "
                f"{pinned_zephyr_version}:SDK_VERSION)"
            ],
            None,
        )
    return [], None


# -------- check 3: repo-wide sdk-ng reference drift -------------------------


def _sdk_ng_url_trigger(zephyr_sdk: dict) -> re.Pattern:
    """Derive the sdk-ng release-URL trigger from `zephyrSdk.baseUrl` at
    run time -- never a second hardcoded copy of the URL path in this gate
    (see module docstring's history section). `baseUrl` is
    "<scheme>://<host>/<org>/<repo>/releases/download/vX.Y.Z/" (enforced by
    the schema); the trigger matches that same prefix with the version path
    segment wildcarded to ANYTHING (a literal `vX.Y.Z` -- the historical
    bypass -- or a GitHub Actions/shell interpolation like
    `v${{ env.SDKV }}` or `v${SDKV}` -- the "wrong variable name" bypass),
    which is why it does not require a digit in that slot the way a
    literal-URL regex would."""
    base_url = zephyr_sdk["baseUrl"]
    m = re.match(r"^(https?://.+/releases/download/)v[^/]+/$", base_url)
    if m is None:
        raise ValueError(
            f"zephyrSdk.baseUrl {base_url!r} doesn't match the "
            f".../releases/download/vX.Y.Z/ shape this gate derives its "
            f"repo-wide scan trigger from -- update _sdk_ng_url_trigger if "
            f"the manifest's baseUrl shape changed"
        )
    return re.compile(re.escape(m.group(1)) + r"v(.+?)/")


def _artifact_filename_triggers(zephyr_sdk: dict) -> list[re.Pattern]:
    """One regex per artifact row in `zephyrSdk.artifacts`, each derived by
    wildcarding the CURRENT pinned version substring out of that artifact's
    own recorded `filename` -- e.g. `zephyr-sdk-1.0.1_linux-x86_64_minimal.
    tar.xz` becomes `zephyr-sdk-(.+?)_linux-x86_64_minimal\\.tar\\.xz`. Never
    a second hardcoded copy of the `zephyr-sdk-<ver>_<host>_minimal.*` /
    `toolchain_gnu_<host>_arm-zephyr-eabi.*` shapes in this gate -- the
    manifest's own filenames are the only place those shapes are spelled
    out."""
    version = zephyr_sdk["version"]
    triggers: list[re.Pattern] = []
    for art in zephyr_sdk["artifacts"]:
        filename = art["filename"]
        if version not in filename:
            continue
        idx = filename.index(version)
        prefix, suffix = filename[:idx], filename[idx + len(version):]
        triggers.append(re.compile(re.escape(prefix) + r"(.+?)" + re.escape(suffix)))
    return triggers


def _iter_workflow_files() -> list[Path]:
    """Every `.github/workflows/*.yml` in the repo -- check 3's actual scan
    scope (module docstring), replacing the curated TOOLCHAIN_WORKFLOWS
    list that check used to define its own scope with. Flat glob only:
    alp-sdk's own workflow directory has no subdirectories."""
    if not WORKFLOWS_DIR.is_dir():
        return []
    return sorted(WORKFLOWS_DIR.glob("*.yml"))


def _check_sdk_reference_drift(
    path: Path,
    url_trigger: re.Pattern,
    filename_triggers: list[re.Pattern],
    manifest_sdk_version: str,
) -> list[str]:
    """Check 3 (module docstring), for one workflow file -- run over EVERY
    `.github/workflows/*.yml`, not a curated list. Independent scans, each
    over every non-comment line:

      1. Any line matching `url_trigger` (the sdk-ng release URL,
         wildcarded past its version path segment) or one of
         `filename_triggers` (a manifest artifact's own filename,
         wildcarded past its version substring) must have EXACTLY the
         canonical manifest-sourced reference
         (`_CANONICAL_VERSION_REF_RE`) in that wildcarded slot -- a literal
         digit sequence or a differently-named variable both fail here,
         since neither trigger regex requires a digit the way a
         literal-URL/name-keyed regex would.
      2. A cache `key:` line naming an "sdk" cache with a literal
         Zephyr-shaped `vX.Y.Z` embedded fails -- the defect-4 category
         confusion. Independent of scan 1: it fires on ANY such line, not
         only ones the trigger regexes above also matched.

    Then, only if EITHER scan 1 trigger fired anywhere in the file (never
    unconditionally -- a workflow that mentions Zephyr but never installs
    the SDK by hand has nothing here to verify):

      3. The file must contain a real read of metadata/toolchains.json
         (`_MANIFEST_READ_RE`) -- catches a workflow that names the RIGHT
         variable (`${{ env.ZEPHYR_SDK_VERSION }}`) but never actually
         populates it from the manifest, i.e. the same hardcode-by-a-
         different-name bypass one level up from scan 1's per-slot check.
      4. The file must reference `ZEPHYR_SDK_SHA256` somewhere -- a
         download with no integrity check at all (the historical
         pr-getting-started-aen801.yml `wget`-with-no-check defect).

    A parse "finding nothing" is never silently treated as "nothing to
    report" for scans 3/4: they are positive absence checks (manifest read
    present, hash reference present), not regex-finds-a-match scans, so
    there is no "no match" case to under-report here.
    """
    if not path.is_file():
        return [f"missing {path.relative_to(REPO).as_posix()}"]
    rel = path.relative_to(REPO).as_posix()
    text = path.read_text(encoding="utf-8")
    problems: list[str] = []
    triggered = False

    labelled_triggers = [("sdk-ng release URL", url_trigger)]
    labelled_triggers += [("artifact filename", p) for p in filename_triggers]

    for lineno, line in enumerate(text.splitlines(), start=1):
        stripped = line.strip()
        if stripped.startswith("#"):
            continue

        for label, pattern in labelled_triggers:
            m = pattern.search(line)
            if m is None:
                continue
            triggered = True
            slot = m.group(1)
            if not _CANONICAL_VERSION_REF_RE.match(slot):
                problems.append(
                    f"{rel}:{lineno}: {label} has version slot {slot!r} -- must be "
                    f"sourced from metadata/toolchains.json (zephyrSdk.version "
                    f"{manifest_sdk_version!r}) via ${{{{ env.ZEPHYR_SDK_VERSION }}}} / "
                    f"${{ZEPHYR_SDK_VERSION}}, not a literal or a differently-named variable"
                )

        if "key:" in line and "sdk" in line.lower():
            m3 = _LITERAL_VERSION_RE.search(line)
            if m3 is not None:
                problems.append(
                    f"{rel}:{lineno}: SDK cache key embeds a literal version "
                    f"{m3.group(0)!r} -- key it on ${{{{ env.ZEPHYR_SDK_VERSION }}}} (sourced "
                    f"from metadata/toolchains.json) instead, not a Zephyr-version literal on "
                    f"an SDK cache"
                )

    if triggered:
        if _MANIFEST_READ_RE.search(text) is None:
            problems.append(
                f"{rel}: names an sdk-ng release URL or artifact filename but never reads "
                f"metadata/toolchains.json anywhere in the file -- ZEPHYR_SDK_VERSION must be "
                f"POPULATED from the manifest, not just correctly named"
            )
        if "ZEPHYR_SDK_SHA256" not in text:
            problems.append(
                f"{rel}: names an sdk-ng release URL or artifact filename with no "
                f"ZEPHYR_SDK_SHA256 verification anywhere in the file"
            )

    return problems


def _check_still_reads_manifest(path: Path) -> list[str]:
    """Check 4 (module docstring): each of TOOLCHAIN_WORKFLOWS must still
    contain a real read of metadata/toolchains.json (`_MANIFEST_READ_RE`).
    A DIFFERENT assertion from `_check_sdk_reference_drift` above: that one
    scans for a drifted COPY of a manifest fact repo-wide; this one polices
    the curated files for still HAVING a manifest read in the first
    place, so deleting just that one step is caught even for a file (like
    pr-twister.yml, which installs the SDK via `west sdk install` and never
    spells out a URL/filename literal at all) that gives
    `_check_sdk_reference_drift` nothing to trigger on."""
    if not path.is_file():
        return [f"missing {path.relative_to(REPO).as_posix()}"]
    text = path.read_text(encoding="utf-8")
    if _MANIFEST_READ_RE.search(text) is not None:
        return []
    rel = path.relative_to(REPO).as_posix()
    return [
        f"{rel}: no longer reads metadata/toolchains.json anywhere -- this is one of the "
        f"workflows TOOLCHAIN_WORKFLOWS asserts must source the Zephyr SDK pin from the "
        f"manifest, and its manifest-read step is gone"
    ]


def _check_artifact_provenance_consistency(manifest: dict) -> list[str]:
    """Check 5 (module docstring, issue #1603): `tier`/`licence` are per-row
    fields (schema `$defs.artifact`) but `component` is the real artifact
    identity -- `minimal-sdk` and `arm-zephyr-eabi-toolchain` are each ONE
    logical upstream artifact repeated across three `host` rows (issue
    #1603's own field placement, chosen to avoid restructuring the existing
    per-host `sizeBytes`/`sha256` shape rather than inventing a
    component-keyed dict). Presence of `tier`/`licence` is already the
    schema's job (`required` in `_load_manifest_and_schema`'s check 1); this
    check catches the drift schema validation cannot: one row's `tier` or
    `licence` edited without its sibling rows, which would silently let a
    consent screen show a different tier or licence for the SAME upstream
    artifact depending only on which host happened to build it. Mirrors the
    two-directional drift bar `_check_artifact_provenance` in the sibling
    `check_bootstrap_manifest.py` (issue #1574) holds `artifactProvenance`
    to, adapted to this file's per-host-row shape instead of that file's
    per-tool-key shape."""
    problems: list[str] = []
    by_component: dict[str, list[dict]] = {}
    for artifact in manifest["zephyrSdk"]["artifacts"]:
        by_component.setdefault(artifact["component"], []).append(artifact)
    for component, rows in sorted(by_component.items()):
        tiers = {row.get("tier") for row in rows}
        licences = {row.get("licence") for row in rows}
        if len(tiers) > 1:
            problems.append(
                f"component {component!r} has disagreeing tier values {sorted(tiers)} "
                f"across its host rows -- one logical artifact must have one tier "
                f"(ADR 0021 SS3, issue #1603)"
            )
        if len(licences) > 1:
            problems.append(
                f"component {component!r} has disagreeing licence values {sorted(licences, key=lambda v: (v is None, v))} "
                f"across its host rows -- one logical artifact must have one licence "
                f"(issue #1603)"
            )
    return problems


def main() -> int:
    manifest, problems = _load_manifest_and_schema()
    if not manifest:
        print("FAIL metadata/toolchains.json", file=sys.stderr)
        for p in problems:
            print(f"  · {p}", file=sys.stderr)
        return 1
    if problems:
        print("FAIL metadata/toolchains.json", file=sys.stderr)
        for p in problems:
            print(f"  · {p}", file=sys.stderr)
        return 1

    problems = []
    skip_notes: list[str] = []

    sdk_problems, skip_reason = _check_sdk_version_matches_zephyr_pin(manifest)
    problems += sdk_problems
    if skip_reason is not None:
        skip_notes.append(skip_reason)

    manifest_sdk_version = manifest["zephyrSdk"]["version"]

    workflow_files = _iter_workflow_files()
    try:
        url_trigger = _sdk_ng_url_trigger(manifest["zephyrSdk"])
        filename_triggers = _artifact_filename_triggers(manifest["zephyrSdk"])
    except ValueError as exc:
        problems.append(str(exc))
    else:
        for wf in workflow_files:
            problems += _check_sdk_reference_drift(
                wf, url_trigger, filename_triggers, manifest_sdk_version
            )

    for wf in TOOLCHAIN_WORKFLOWS:
        problems += _check_still_reads_manifest(wf)

    problems += _check_artifact_provenance_consistency(manifest)

    if problems:
        print(f"FAIL toolchain lock drift (metadata/toolchains.json declares "
              f"zephyrSdk.version {manifest_sdk_version!r}):", file=sys.stderr)
        for p in problems:
            print(f"  · {p}", file=sys.stderr)
        return 1

    for note in skip_notes:
        print(f"check_toolchain_lock: SKIP -- {note}")
    print(f"check_toolchain_lock: OK -- metadata/toolchains.json agrees with "
          f"{len(workflow_files)} .github/workflows/*.yml file(s) (repo-wide scan) and the "
          f"{len(TOOLCHAIN_WORKFLOWS)} curated TOOLCHAIN_WORKFLOWS still read the manifest; "
          f"Zephyr SDK {manifest_sdk_version}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
