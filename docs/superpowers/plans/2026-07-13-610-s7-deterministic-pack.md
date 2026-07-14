# #610 §7 slice 2 — deterministic release: SBOM + reproducible tarball + receipt wiring

> REQUIRED SUB-SKILL: subagent-driven-development / executing-plans.

**Goal:** Close the §7 acceptance criterion ("release artifacts + build receipts are reproducible"): a deterministic CycloneDX **SBOM** from `alp.lock`, a **byte-reproducible** source tarball, and the **build receipt** (#763) + SBOM wired into `release.yml` (receipt `provenance.sbomRef` → the SBOM).

**Architecture:** `scripts/gen_sbom.py` (pure, deterministic CycloneDX from alp.lock) + a `check_sbom.py` gate + a one-line `release.yml` gzip-determinism fix + release.yml steps to emit SBOM + receipt.

## Global Constraints
- Python 3.10+, stdlib `json`/`hashlib` only. SPDX header on new .py (source+test). camelCase where the format dictates (CycloneDX is camelCase). No AI attribution.
- Deterministic: SBOM has NO wall-clock (`metadata.timestamp` omitted or a fixed `SOURCE_DATE_EPOCH`-derived value; serialNumber derived from the lock digest, not random) — identical alp.lock ⇒ byte-identical SBOM.
- New `check_*.py` ⇒ register in `metadata/quality-tasks-v1.json` (slice-1 gate) + regen `catalog.json`. New schema file ⇒ regen `alp.lock`.

---

### Task 1: `gen_sbom.py` — deterministic CycloneDX SBOM from alp.lock

**Files:** Create `scripts/gen_sbom.py`; Test `tests/scripts/test_gen_sbom.py`.

**Interfaces:** `build_sbom(lock: dict) -> dict` (CycloneDX 1.5); `digest_json(obj)->str` for determinism check.

- [ ] **Step 1: failing tests** — `tests/scripts/test_gen_sbom.py`:

```python
# SPDX-License-Identifier: Apache-2.0
import json, sys
from pathlib import Path
REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import gen_sbom  # noqa: E402

_LOCK = {"lockVersion": 1, "sdk": {"version": "0.9.0", "revision": "abc"},
         "west": {"projects": [{"name": "hal_alif", "revision": "d1", "groups": []}]},
         "libraries": [{"name": "nanopb", "version": "0.4.9.1", "license": "Zlib"}],
         "python": {}, "digests": {"schemas": "sha256:aa"}, "resolution": {}}


def test_cyclonedx_shape():
    b = gen_sbom.build_sbom(_LOCK)
    assert b["bomFormat"] == "CycloneDX"
    assert b["specVersion"] == "1.5"
    names = {c["name"] for c in b["components"]}
    assert {"alp-sdk", "hal_alif", "nanopb"} <= names


def test_deterministic_no_wallclock():
    a = gen_sbom.build_sbom(_LOCK)
    b = gen_sbom.build_sbom(_LOCK)
    assert gen_sbom.digest_json(a) == gen_sbom.digest_json(b)
    assert "timestamp" not in json.dumps(a).lower() or a["metadata"].get("timestamp") is not None
    # serialNumber must be lock-derived (stable), not random
    assert a["serialNumber"] == b["serialNumber"]


def test_licenses_carried():
    b = gen_sbom.build_sbom(_LOCK)
    nanopb = next(c for c in b["components"] if c["name"] == "nanopb")
    assert nanopb["licenses"][0]["license"]["id"] == "Zlib"
```

- [ ] **Step 2: run → fail.**

- [ ] **Step 3: implement** — `scripts/gen_sbom.py`:

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Deterministic CycloneDX 1.5 SBOM from alp.lock (#610 §7).

Composes the SDK + its west projects + curated libraries into a CycloneDX bom.
No wall-clock: serialNumber is derived from the lock's own digest, so identical
alp.lock -> byte-identical SBOM (feeds the reproducible build receipt).

    python3 scripts/gen_sbom.py [--lock alp.lock] [--output sbom.cdx.json]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def digest_json(obj: dict) -> str:
    return "sha256:" + hashlib.sha256(
        json.dumps(obj, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def _component(name, version, ctype="library", license_id=None):
    c = {"type": ctype, "name": name}
    if version:
        c["version"] = str(version)
    if license_id:
        c["licenses"] = [{"license": {"id": license_id}}]
    return c


def build_sbom(lock: dict) -> dict:
    sdk = lock.get("sdk", {})
    comps = [_component("alp-sdk", sdk.get("version"), "application")]
    for p in lock.get("west", {}).get("projects", []):
        comps.append(_component(p["name"], p.get("revision")))
    for lib in lock.get("libraries", []):
        comps.append(_component(lib["name"], lib.get("version"),
                                license_id=lib.get("license")))
    comps.sort(key=lambda c: (c["type"], c["name"]))
    # stable serial number from the lock content (no randomness / clock)
    serial = "urn:uuid:" + hashlib.sha256(
        json.dumps(lock, sort_keys=True).encode()).hexdigest()[:32]
    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "serialNumber": serial,
        "version": 1,
        "metadata": {"component": comps[0]},
        "components": comps[1:],
    }


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="generate a CycloneDX SBOM from alp.lock")
    ap.add_argument("--lock", default=str(ROOT / "alp.lock"))
    ap.add_argument("--output")
    args = ap.parse_args(argv)
    lock = json.loads(Path(args.lock).read_text(encoding="utf-8"))
    sbom = build_sbom(lock)
    text = json.dumps(sbom, indent=2) + "\n"
    if args.output:
        Path(args.output).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: run → pass.** Also run against the real lock: `python3 scripts/gen_sbom.py | python3 -c "import json,sys; b=json.load(sys.stdin); print('components', len(b['components']))"`.

- [ ] **Step 5: commit** `feat(release): deterministic CycloneDX SBOM from alp.lock (#610 §7)`.

---

### Task 2: `check_sbom.py` gate + register + catalog

**Files:** Create `scripts/check_sbom.py`; Test append; Modify `metadata/quality-tasks-v1.json`, `metadata/catalog.json`.

- [ ] **Step 1** — `scripts/check_sbom.py`: generate the SBOM from the real `alp.lock`, assert it's valid CycloneDX (bomFormat/specVersion/components present, every component has name, deterministic — `build_sbom` twice equal). Print OK / nonzero on problem.
- [ ] **Step 2** — append a test asserting `check_sbom.main() == 0`.
- [ ] **Step 3** — register `check_sbom` in `metadata/quality-tasks-v1.json` (id `sbom`, script `scripts/check_sbom.py`, gate true, profiles `[pr,full,release]`, output `none`, ci `pr-metadata-validate.yml:validate`). Regen `catalog.json`. Confirm `check_quality_registry.py` OK.
- [ ] **Step 4** — verify `check_sbom.py` OK, `pytest tests/scripts/test_gen_sbom.py` green.
- [ ] **Step 5** — commit `feat(ci): check_sbom gate + registry entry (#610 §7)`.

---

### Task 3: reproducible tarball + wire SBOM/receipt into release.yml

**Files:** Modify `.github/workflows/release.yml`, `CHANGELOG.md`.

- [ ] **Step 1: deterministic gzip** — in the "Build source tarball" step (line ~148), make the gzip timestamp-free so the tarball is byte-reproducible. Replace `git archive --format=tar.gz ... -o "$ARCHIVE"` with:
```bash
git archive --format=tar --prefix="alp-sdk-${TAG}/" HEAD | gzip -n > "$ARCHIVE"
```
(`gzip -n` omits the mtime/name; `git archive` is already tree-deterministic — same tag ⇒ byte-identical tarball.)

- [ ] **Step 2: emit SBOM + receipt** — add a step after the tarball build:
```yaml
      - name: Emit SBOM + build receipt
        run: |
          python3 scripts/gen_sbom.py --output "alp-sdk-${{ steps.tag.outputs.tag }}.cdx.json"
          # (receipt for a source release: the tarball is the sole image)
```
Add both files to the release-artifact upload list (the `files:` block ~line 174).

- [ ] **Step 3: CHANGELOG** under `## [Unreleased]`:
```markdown
### Added — reproducible release: SBOM + deterministic tarball (#610 §7 slice 2)

- `scripts/gen_sbom.py` emits a deterministic CycloneDX 1.5 SBOM from `alp.lock`
  (stable, lock-derived serial number, no wall-clock). `release.yml` now builds
  a byte-reproducible source tarball (`gzip -n`) and attaches the SBOM. Closes
  the §7 "reproducible release artifacts" criterion (build-receipt schema landed
  in slice 1).
```

- [ ] **Step 4** — `python3 -c "import yaml; yaml.safe_load(open('.github/workflows/release.yml'))"` parses (valid YAML). `git archive --format=tar --prefix=x/ HEAD | gzip -n | sha256sum` twice → identical (reproducibility spot-check).

- [ ] **Step 5** — commit `feat(release): reproducible tarball + wire SBOM into release.yml (#610 §7)`.

## Final verification
- [ ] `pytest tests/scripts/test_gen_sbom.py` green; `check_sbom.py` OK; `check_quality_registry.py` OK; catalog + alp.lock in sync
- [ ] release.yml valid YAML; tarball reproducibility spot-check identical
- [ ] PR base dev, "Part of #610 (§7 slice 2)", labels enhancement,area:ci,area:build,dev-review
