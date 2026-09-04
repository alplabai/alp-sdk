# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_toolchain_lock.py.

The gate is regex/schema-driven against metadata/toolchains.json + its
schema + metadata/bootstrap.json + a repo-wide `.github/workflows/*.yml`
scan (plus a positive assertion over the curated workflows and a live
`git show <rev>:SDK_VERSION` cross-check against a pinned Zephyr checkout).
Each test here mutates a TEMP COPY of that corpus (or a throwaway fake
Zephyr git repo) and asserts the gate actually fires for the documented
failure mode -- a green run on the real repo alone proves nothing about
whether the gate catches drift.

Run locally:

    python -m pytest tests/scripts/test_check_toolchain_lock.py -q
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_toolchain_lock.py"

sys.path.insert(0, str(REPO / "scripts"))
import check_toolchain_lock as gate  # noqa: E402

# The exact relative-path corpus the gate reads (mirrors
# gate.TOOLCHAIN_WORKFLOWS + its other module-level Path constants). This is
# also the full set of files `_iter_workflow_files` sees in the scaffolded
# tmp_path corpus -- tests that need an EXTRA, non-curated workflow to
# exercise the repo-wide scan add one explicitly (see `_add_workflow`).
_CORPUS_RELPATHS = [
    "metadata/toolchains.json",
    "metadata/schemas/toolchains-v1.schema.json",
    "metadata/bootstrap.json",
    ".github/workflows/pr-getting-started-aen801.yml",
    ".github/workflows/pr-twister.yml",
]


def _scaffold(tmp_path: Path) -> None:
    """Copy the real corpus into tmp_path byte-for-byte -- tests mutate
    this COPY, never the real repo."""
    for rel in _CORPUS_RELPATHS:
        src = REPO / rel
        dst = tmp_path / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def _point_gate_at(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(sys, "argv", ["check_toolchain_lock.py"])
    monkeypatch.setattr(gate, "REPO", tmp_path)
    monkeypatch.setattr(gate, "MANIFEST", tmp_path / "metadata/toolchains.json")
    monkeypatch.setattr(gate, "SCHEMA", tmp_path / "metadata/schemas/toolchains-v1.schema.json")
    monkeypatch.setattr(gate, "BOOTSTRAP_MANIFEST", tmp_path / "metadata/bootstrap.json")
    monkeypatch.setattr(gate, "WORKFLOWS_DIR", tmp_path / ".github/workflows")
    monkeypatch.setattr(gate, "TOOLCHAIN_WORKFLOWS", [
        tmp_path / ".github/workflows/pr-getting-started-aen801.yml",
        tmp_path / ".github/workflows/pr-twister.yml",
    ])
    # The SDK/Zephyr cross-check is exercised by its own dedicated tests
    # below (with a throwaway fake Zephyr git repo); default it to
    # "unresolvable" here so every workflow-drift test isn't coupled to
    # whatever Zephyr checkout (if any) happens to sit next to this repo
    # on the machine running the suite.
    monkeypatch.setattr(gate, "_resolve_zephyr_dir", lambda: tmp_path / "no-such-zephyr-checkout")
    monkeypatch.delenv("ALP_REQUIRE_ZEPHYR_ORACLE", raising=False)


def _add_workflow(tmp_path: Path, name: str, text: str) -> Path:
    """Drop an EXTRA workflow file into the scaffolded corpus -- the exact
    shape of the real bypass this change closes: a workflow
    TOOLCHAIN_WORKFLOWS never named, dropped straight into
    .github/workflows/, that the repo-wide scan must still cover."""
    path = tmp_path / ".github/workflows" / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def _edit_manifest(tmp_path: Path, mutate) -> None:
    p = tmp_path / "metadata/toolchains.json"
    data = json.loads(p.read_text(encoding="utf-8"))
    mutate(data)
    p.write_text(json.dumps(data, indent=2), encoding="utf-8")


def _replace(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    assert old in text, f"fixture assumption broken: {old!r} not found in {path}"
    path.write_text(text.replace(old, new), encoding="utf-8")


def _make_fake_zephyr_repo(tmp_path: Path, sdk_version_at_tag: str, tag: str) -> Path:
    """A throwaway git repo standing in for a Zephyr checkout, with
    `SDK_VERSION` == `sdk_version_at_tag` committed and tagged `tag` --
    lets tests exercise `git show <tag>:SDK_VERSION` without a real,
    multi-hundred-MB Zephyr clone."""
    zephyr_dir = tmp_path / "fake-zephyr"
    zephyr_dir.mkdir()
    run = lambda *args: subprocess.run(  # noqa: E731
        ["git", *args], cwd=zephyr_dir, check=True, capture_output=True, text=True,
    )
    run("init", "-q")
    run("config", "user.email", "test@example.invalid")
    run("config", "user.name", "test")
    (zephyr_dir / "SDK_VERSION").write_text(sdk_version_at_tag + "\n", encoding="utf-8")
    run("add", "SDK_VERSION")
    run("commit", "-q", "-m", "seed")
    run("tag", tag)
    return zephyr_dir


def test_default_corpus_passes():
    proc = subprocess.run(
        [sys.executable, str(SCRIPT)], capture_output=True, text=True,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_scaffolded_copy_passes_unmodified(tmp_path, monkeypatch, capsys):
    """Sanity check for the scaffold/monkeypatch machinery itself: an
    untouched copy of the real corpus must also pass, or every
    failure-mode test below would be meaningless."""
    _scaffold(tmp_path)
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out
    assert "OK" in out


# ---------------------------------------------------------------------
# 1. Schema validation
# ---------------------------------------------------------------------


def test_schema_missing_required_key_fails(tmp_path, monkeypatch, capsys):
    _scaffold(tmp_path)
    _edit_manifest(tmp_path, lambda d: d.pop("measuredFootprint"))
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "schema:" in err


def test_schema_bad_sha256_shape_fails(tmp_path, monkeypatch, capsys):
    _scaffold(tmp_path)
    _edit_manifest(
        tmp_path,
        lambda d: d["zephyrSdk"]["artifacts"].__setitem__(
            0, {**d["zephyrSdk"]["artifacts"][0], "sha256": "not-a-real-hash"}
        ),
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "schema:" in err


# ---------------------------------------------------------------------
# 2. zephyrSdk.version <-> Zephyr's own SDK_VERSION at the pinned revision
# ---------------------------------------------------------------------


def test_sdk_version_matches_real_zephyr_pin_passes(tmp_path, monkeypatch):
    manifest = {"zephyrSdk": {"version": "1.0.1"}}
    fake_zephyr = _make_fake_zephyr_repo(tmp_path, "1.0.1", "v4.4.1")
    monkeypatch.setattr(gate, "_pinned_zephyr_version", lambda: "v4.4.1")
    monkeypatch.setattr(gate, "_resolve_zephyr_dir", lambda: fake_zephyr)
    monkeypatch.delenv("ALP_REQUIRE_ZEPHYR_ORACLE", raising=False)
    problems, skip_reason = gate._check_sdk_version_matches_zephyr_pin(manifest)
    assert problems == []
    assert skip_reason is None


def test_sdk_version_disagreement_with_real_zephyr_pin_fails(tmp_path, monkeypatch):
    """The exact regression this check exists for: a manifest SDK version
    (e.g. the historical 0.16.8-shaped mistake) that disagrees with the
    real SDK_VERSION file at the pinned Zephyr revision."""
    manifest = {"zephyrSdk": {"version": "0.16.8"}}
    fake_zephyr = _make_fake_zephyr_repo(tmp_path, "1.0.1", "v4.4.1")
    monkeypatch.setattr(gate, "_pinned_zephyr_version", lambda: "v4.4.1")
    monkeypatch.setattr(gate, "_resolve_zephyr_dir", lambda: fake_zephyr)
    monkeypatch.delenv("ALP_REQUIRE_ZEPHYR_ORACLE", raising=False)
    problems, skip_reason = gate._check_sdk_version_matches_zephyr_pin(manifest)
    assert skip_reason is None
    assert len(problems) == 1
    assert "0.16.8" in problems[0]
    assert "1.0.1" in problems[0]
    assert "SDK_VERSION" in problems[0]


def test_sdk_version_check_skips_with_no_zephyr_checkout(tmp_path, monkeypatch):
    manifest = {"zephyrSdk": {"version": "1.0.1"}}
    monkeypatch.setattr(gate, "_pinned_zephyr_version", lambda: "v4.4.1")
    monkeypatch.setattr(gate, "_resolve_zephyr_dir", lambda: tmp_path / "does-not-exist")
    monkeypatch.delenv("ALP_REQUIRE_ZEPHYR_ORACLE", raising=False)
    problems, skip_reason = gate._check_sdk_version_matches_zephyr_pin(manifest)
    assert problems == []
    assert skip_reason is not None
    assert "no Zephyr checkout resolved" in skip_reason


def test_sdk_version_check_hard_fails_when_oracle_required_but_absent(tmp_path, monkeypatch):
    manifest = {"zephyrSdk": {"version": "1.0.1"}}
    monkeypatch.setattr(gate, "_pinned_zephyr_version", lambda: "v4.4.1")
    monkeypatch.setattr(gate, "_resolve_zephyr_dir", lambda: tmp_path / "does-not-exist")
    monkeypatch.setenv("ALP_REQUIRE_ZEPHYR_ORACLE", "1")
    problems, skip_reason = gate._check_sdk_version_matches_zephyr_pin(manifest)
    assert skip_reason is None
    assert len(problems) == 1
    assert "ALP_REQUIRE_ZEPHYR_ORACLE=1" in problems[0]


def test_sdk_version_check_uses_git_show_not_working_tree_checkout(tmp_path, monkeypatch):
    """The oracle reads the pinned revision via `git show <rev>:SDK_VERSION`
    from the object store, NOT the working tree's currently-checked-out
    ref -- a checkout currently sitting on a different tag must still
    resolve correctly as long as the pinned tag exists as a git object."""
    manifest = {"zephyrSdk": {"version": "1.0.1"}}
    fake_zephyr = _make_fake_zephyr_repo(tmp_path, "1.0.1", "v4.4.1")
    # Move HEAD to a second commit/tag so the working tree is NOT checked
    # out at v4.4.1 any more, mirroring a stale local dev clone.
    (fake_zephyr / "SDK_VERSION").write_text("9.9.9\n", encoding="utf-8")
    subprocess.run(["git", "add", "SDK_VERSION"], cwd=fake_zephyr, check=True, capture_output=True)
    subprocess.run(
        ["git", "commit", "-q", "-m", "later"], cwd=fake_zephyr, check=True, capture_output=True,
    )
    subprocess.run(["git", "tag", "v4.5.0"], cwd=fake_zephyr, check=True, capture_output=True)
    monkeypatch.setattr(gate, "_pinned_zephyr_version", lambda: "v4.4.1")
    monkeypatch.setattr(gate, "_resolve_zephyr_dir", lambda: fake_zephyr)
    monkeypatch.delenv("ALP_REQUIRE_ZEPHYR_ORACLE", raising=False)
    problems, skip_reason = gate._check_sdk_version_matches_zephyr_pin(manifest)
    assert problems == []
    assert skip_reason is None


# ---------------------------------------------------------------------
# 3. Repo-wide CI workflow SDK-reference drift (not a curated scan scope)
# ---------------------------------------------------------------------


def test_bypass_a_new_workflow_hardcoded_curl_no_hash_fails(tmp_path, monkeypatch, capsys):
    """Bypass A (the serious one, reported live): a brand NEW workflow file
    -- one TOOLCHAIN_WORKFLOWS never named -- with `curl -sSL -O`, a
    hardcoded `ZEPHYR_SDK_VERSION=0.16.8`, a hardcoded sdk-ng URL +
    filename, and NO sha256 check at all. The curated four-file scan this
    change replaces passed this cleanly; the repo-wide scan must not."""
    _scaffold(tmp_path)
    _add_workflow(
        tmp_path, "pr-new-bypass-a.yml",
        "name: bypass-a\n"
        "on: [pull_request]\n"
        "jobs:\n"
        "  install:\n"
        "    runs-on: ubuntu-latest\n"
        "    env:\n"
        "      ZEPHYR_SDK_VERSION: 0.16.8\n"
        "    steps:\n"
        "      - name: Install Zephyr SDK the wrong way\n"
        "        run: |\n"
        "          curl -sSL -O https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.16.8/zephyr-sdk-0.16.8_linux-x86_64_minimal.tar.xz\n"
        "          tar -xJf zephyr-sdk-0.16.8_linux-x86_64_minimal.tar.xz\n",
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "pr-new-bypass-a.yml" in err
    assert "0.16.8" in err
    assert "not a literal or a differently-named variable" in err
    assert "never reads metadata/toolchains.json" in err
    assert "no ZEPHYR_SDK_SHA256 verification" in err


def test_bypass_b_env_default_and_interpolated_url_fails(tmp_path, monkeypatch, capsys):
    """Bypass B (reported live): the version supplied only as an `env:`
    default under an UNRELATED name (`SDKV`), with the URL built from
    `${SDKV}` -- no literal digit follows an `=`/`:` anywhere, so a
    digit-keyed regex would miss this entirely. The URL/filename-SHAPE
    trigger (wildcarded past the version slot, no digit required) must
    catch it regardless of the variable's name."""
    _scaffold(tmp_path)
    _add_workflow(
        tmp_path, "pr-new-bypass-b.yml",
        "name: bypass-b\n"
        "on: [pull_request]\n"
        "jobs:\n"
        "  install:\n"
        "    runs-on: ubuntu-latest\n"
        "    env:\n"
        "      SDKV: 0.16.8\n"
        "    steps:\n"
        "      - name: Install Zephyr SDK via an env default\n"
        "        run: |\n"
        '          curl -sSL -O "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${SDKV}/zephyr-sdk-${SDKV}_linux-x86_64_minimal.tar.xz"\n',
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "pr-new-bypass-b.yml" in err
    assert "${SDKV}" in err
    assert "not a literal or a differently-named variable" in err


def test_manifest_sourced_env_var_with_hash_check_passes(tmp_path, monkeypatch, capsys):
    """The real, correct pattern -- a NEW workflow (not one of the curated
    ones) that reads metadata/toolchains.json, references the version/URL
    only via the canonical `${ZEPHYR_SDK_VERSION}` / `${ZEPHYR_SDK_URL}`
    forms, and verifies `ZEPHYR_SDK_SHA256` -- must PASS. This is the
    no-false-positive proof for the repo-wide scan."""
    _scaffold(tmp_path)
    _add_workflow(
        tmp_path, "pr-new-good.yml",
        "name: good\n"
        "on: [pull_request]\n"
        "jobs:\n"
        "  install:\n"
        "    runs-on: ubuntu-latest\n"
        "    steps:\n"
        "      - name: Read Zephyr SDK toolchain pin (metadata/toolchains.json)\n"
        "        run: |\n"
        "          python3 -c '\n"
        "          import json\n"
        '          d = json.load(open("metadata/toolchains.json"))\n'
        '          sdk = d["zephyrSdk"]\n'
        '          art = next(a for a in sdk["artifacts"] if a["host"] == "linux-x86_64" and a["component"] == "minimal-sdk")\n'
        '          print(f"ZEPHYR_SDK_VERSION={sdk[\\"version\\"]}")\n'
        '          print(f"ZEPHYR_SDK_URL={sdk[\\"baseUrl\\"] + art[\\"filename\\"]}")\n'
        '          print(f"ZEPHYR_SDK_SHA256={art[\\"sha256\\"]}")\n'
        "          ' >> \"$GITHUB_ENV\"\n"
        "      - name: Install Zephyr SDK\n"
        "        run: |\n"
        '          curl -fsSL -o /tmp/zephyr-sdk.tar.xz "${ZEPHYR_SDK_URL}"\n'
        '          ACTUAL_SHA256="$(sha256sum /tmp/zephyr-sdk.tar.xz | awk \'{print $1}\')"\n'
        '          if [ "${ACTUAL_SHA256}" != "${ZEPHYR_SDK_SHA256}" ]; then\n'
        '            echo "::error::sha256 mismatch for zephyr-sdk-${ZEPHYR_SDK_VERSION}_linux-x86_64_minimal.tar.xz"\n'
        "            exit 1\n"
        "          fi\n",
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    captured = capsys.readouterr()
    assert rv == 0, captured.err
    assert "OK" in captured.out


def test_prose_comment_quoting_version_is_not_flagged(tmp_path, monkeypatch, capsys):
    """The exact false-positive class the retired curated-scan rationale
    feared ("a repo-wide scan ... would need its own allowlist for false
    positives -- renode's own advisory-gate comments quote SDK versions as
    prose"): a comment naming a bare version, with no sdk-ng URL and no
    artifact filename anywhere. Must stay green with ZERO allowlist
    entries -- this is the proof that fear doesn't hold once the trigger
    is the URL/filename shape rather than a bare version verb/number."""
    _scaffold(tmp_path)
    _add_workflow(
        tmp_path, "pr-new-prose.yml",
        "# This CI intentionally does not install the Zephyr SDK (1.0.1) or\n"
        "# any cross toolchain -- see metadata/toolchains.json for the pin\n"
        "# used elsewhere. Native_sim builds use the host gcc directly.\n"
        "name: prose\n"
        "on: [pull_request]\n"
        "jobs:\n"
        "  build:\n"
        "    runs-on: ubuntu-latest\n"
        "    steps:\n"
        "      - run: echo \"nothing to install here\"\n",
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    captured = capsys.readouterr()
    assert rv == 0, captured.err
    assert "OK" in captured.out


def test_sdk_cache_key_category_confusion_fails(tmp_path, monkeypatch, capsys):
    """The exact defect-4 regression: an SDK cache key naming the
    *Zephyr* version instead of `${{ env.ZEPHYR_SDK_VERSION }}`. Repo-wide
    now, not curated-scope, but pr-twister.yml (one of the curated
    workflows) is still where this literal lives."""
    _scaffold(tmp_path)
    _replace(
        tmp_path / ".github/workflows/pr-twister.yml",
        "key: zephyr-sdk-arm-zephyr-eabi-${{ env.ZEPHYR_SDK_VERSION }}-${{ runner.os }}",
        "key: zephyr-sdk-arm-zephyr-eabi-v4.4.0-${{ runner.os }}",
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "pr-twister.yml" in err
    assert "SDK cache key embeds a literal version" in err
    assert "v4.4.0" in err


def test_zephyr_only_cache_key_not_flagged_as_sdk_confusion(tmp_path, monkeypatch, capsys):
    """The Zephyr *checkout* cache key (a different, legitimate concept
    check_bootstrap_manifest.py already gates) must NOT be flagged here --
    it names no 'sdk' substring, only 'zephyr'."""
    _scaffold(tmp_path)
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0
    assert "OK" in out


def test_sdk_url_without_sha256_verification_fails(tmp_path, monkeypatch, capsys):
    """A file that fetches the SDK archive by URL but never verifies it --
    defect 3's exact shape (the historical wget-with-no-check).
    pr-getting-started-aen801.yml is the vehicle because it is the corpus's
    only remaining workflow that fetches the archive by URL at all
    (pr-twister.yml installs via `west sdk install`); this test used
    pr-renode-aen-smoke.yml until docs/adr/0022 Amendment 2 deleted it."""
    _scaffold(tmp_path)
    wf = tmp_path / ".github/workflows/pr-getting-started-aen801.yml"
    text = wf.read_text(encoding="utf-8")
    assert "ZEPHYR_SDK_SHA256" in text, "fixture assumption broken: no sha256 check to remove"
    wf.write_text(text.replace("ZEPHYR_SDK_SHA256", "SOME_OTHER_NAME"), encoding="utf-8")
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "pr-getting-started-aen801.yml" in err
    assert "no ZEPHYR_SDK_SHA256 verification" in err


def test_missing_workflow_fails(tmp_path, monkeypatch, capsys):
    _scaffold(tmp_path)
    (tmp_path / ".github/workflows/pr-twister.yml").unlink()
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "missing" in err
    assert "pr-twister.yml" in err


# ---------------------------------------------------------------------
# 4. TOOLCHAIN_WORKFLOWS positive assertion: must still read the manifest
# ---------------------------------------------------------------------


def test_curated_workflow_manifest_read_deleted_fails(tmp_path, monkeypatch, capsys):
    """Item 2's whole reason to exist: TOOLCHAIN_WORKFLOWS is repurposed
    from "the scan scope" to a positive assertion that each curated
    workflow still contains a manifest read. pr-twister.yml never names an sdk-ng
    URL/filename literal at all (it installs via `west sdk install`), so
    the repo-wide scan (check 3) has NOTHING to trigger on here -- deleting
    its manifest-read step must still fail, and only this separate check
    can catch it."""
    _scaffold(tmp_path)
    path = tmp_path / ".github/workflows/pr-twister.yml"
    text = path.read_text(encoding="utf-8")
    assert 'd = json.load(open("metadata/toolchains.json"))' in text
    text = text.replace(
        'd = json.load(open("metadata/toolchains.json"))',
        'd = {"zephyrSdk": {"version": "1.0.1"}}  # manifest read removed',
    )
    path.write_text(text, encoding="utf-8")
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "pr-twister.yml" in err
    assert "no longer reads metadata/toolchains.json" in err


# ---------------------------------------------------------------------
# 5. tier/licence (issue #1603)
# ---------------------------------------------------------------------


def test_artifact_missing_tier_fails_schema(tmp_path, monkeypatch, capsys):
    """Presence of `tier` is enforced by the schema (check 1), the same way
    `sha256` already is -- proves the field is actually REQUIRED, not just
    documented."""
    _scaffold(tmp_path)
    _edit_manifest(
        tmp_path,
        lambda d: d["zephyrSdk"]["artifacts"].__setitem__(
            0, {k: v for k, v in d["zephyrSdk"]["artifacts"][0].items() if k != "tier"}
        ),
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "schema:" in err


def test_artifact_missing_licence_fails_schema(tmp_path, monkeypatch, capsys):
    _scaffold(tmp_path)
    _edit_manifest(
        tmp_path,
        lambda d: d["zephyrSdk"]["artifacts"].__setitem__(
            0, {k: v for k, v in d["zephyrSdk"]["artifacts"][0].items() if k != "licence"}
        ),
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "schema:" in err


def test_artifact_bad_tier_value_fails_schema(tmp_path, monkeypatch, capsys):
    """`tier` is a closed enum (A/B/C) -- a made-up tier must fail, not
    silently pass through as a string."""
    _scaffold(tmp_path)
    _edit_manifest(
        tmp_path,
        lambda d: d["zephyrSdk"]["artifacts"].__setitem__(
            0, {**d["zephyrSdk"]["artifacts"][0], "tier": "Z"}
        ),
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "schema:" in err


def test_tier_disagreement_across_host_rows_for_same_component_fails(tmp_path, monkeypatch, capsys):
    """The exact drift `_check_artifact_provenance_consistency` exists for:
    schema validation alone would happily accept a `minimal-sdk` row with
    tier A on linux and tier B on windows -- both are individually valid
    enum members. This is the regression only the new check catches."""
    _scaffold(tmp_path)

    def _mutate(d):
        artifacts = d["zephyrSdk"]["artifacts"]
        for i, row in enumerate(artifacts):
            if row["component"] == "minimal-sdk" and row["host"] == "windows-x86_64":
                artifacts[i] = {**row, "tier": "B"}

    _edit_manifest(tmp_path, _mutate)
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "minimal-sdk" in err
    assert "disagreeing tier" in err


def test_licence_disagreement_across_host_rows_for_same_component_fails(tmp_path, monkeypatch, capsys):
    _scaffold(tmp_path)

    def _mutate(d):
        artifacts = d["zephyrSdk"]["artifacts"]
        for i, row in enumerate(artifacts):
            if row["component"] == "arm-zephyr-eabi-toolchain" and row["host"] == "macos-aarch64":
                artifacts[i] = {**row, "licence": "GPL-3.0-or-later"}

    _edit_manifest(tmp_path, _mutate)
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "arm-zephyr-eabi-toolchain" in err
    assert "disagreeing licence" in err


def test_consistent_tier_and_licence_across_host_rows_passes():
    """Direct unit test of the helper (not the whole gate) proving it
    accepts the consistent real shape -- every row tier A, licence null."""
    manifest = {
        "zephyrSdk": {
            "artifacts": [
                {"host": "linux-x86_64", "component": "minimal-sdk", "tier": "A", "licence": None},
                {"host": "windows-x86_64", "component": "minimal-sdk", "tier": "A", "licence": None},
                {"host": "macos-aarch64", "component": "minimal-sdk", "tier": "A", "licence": None},
            ]
        }
    }
    assert gate._check_artifact_provenance_consistency(manifest) == []
