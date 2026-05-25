# Linux Build — Phase 2: Unified `alp build` front-end

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One `alp build` command, driven by `board.yaml`, builds every supported SoM's Zephyr (M-core) + Linux (A-core) slices — the customer never invokes `bitbake-layers` / `TEMPLATECONF` / `west` directly.

**Architecture:** A click subcommand resolves `board.yaml` via `alp_orchestrate.load_board_yaml()`, then a dispatcher routes each `Slice` to the `ZephyrBuilder` (`os=zephyr`) or a per-SoC-family `BspProvider` (`os=yocto`). The provider is **manifest-driven** (generic `prepare`/`bake`); BSP specifics live in `metadata/bsp/<family>.yaml`. Unit tests mock `subprocess`; only the integration bake needs a real BSP.

**Tech Stack:** Python 3, click (existing `alp` CLI), pytest + `unittest.mock`, pyyaml. Reuses `scripts/alp_orchestrate.py` (`load_board_yaml`, `Slice`, `BoardProject`).

**Environment legend:** 🖥️ DEV-BOX (Tasks 0–7, all unit-tested with mocked bitbake) · 🔧 BENCH (Task 8 integration bake only). Depends on Phase-1 having proven the native 7.10 flow.

**File structure:**
- Create: `scripts/alp_cli/bsp/{__init__.py,manifest.py,locate.py,base.py,renesas_rzv2n.py,nxp_imx93.py}`, `scripts/alp_cli/zephyr_build.py`, `scripts/alp_cli/build.py`
- Modify: `scripts/alp_cli/main.py` (register `build`)
- Create: `metadata/bsp/renesas-rzv2n.yaml`, `metadata/bsp/nxp-imx93.yaml`
- Test: `tests/scripts/alp_cli/test_bsp_manifest.py`, `test_bsp_locate.py`, `test_bsp_registry.py`, `test_renesas_provider.py`, `test_dispatch.py`, `test_build_cli.py`

---

### Task 0 🖥️ — Recon: confirm the orchestrator API

- [ ] Verify in `scripts/alp_orchestrate.py`: `load_board_yaml(path) -> BoardProject`; `BoardProject.cores: dict[str, Slice]`; `Slice` has `core_id, os, machine, image`. (Grounded from the module docstring + dataclasses; if a field name differs, adjust Tasks 5–6.)

---

### Task 1 🖥️ — BSP manifest + loader

**Files:** Create `metadata/bsp/renesas-rzv2n.yaml`, `scripts/alp_cli/bsp/__init__.py`, `scripts/alp_cli/bsp/manifest.py`; Test `tests/scripts/alp_cli/test_bsp_manifest.py`

- [ ] **Step 1: Write `metadata/bsp/renesas-rzv2n.yaml`:**
```yaml
family: renesas-rzv2n
bsp: { package_id: RTK0EF0045Z94001AZJ, version: v1.0.3, ai_sdk: "7.10", yocto: scarthgap-5.0.11 }
templateconf: meta-renesas/meta-rz-distro/conf/templates/vlp-v4-conf
machine_pattern: "e1m-{sku}-a55"
layers:
  - meta-rz-features/meta-rz-graphics
  - meta-rz-features/meta-rz-drpai
  - meta-rz-features/meta-rz-opencva
  - meta-rz-features/meta-rz-codecs
  - meta-econsys
  - meta-ros/meta-ros2-humble
  - alp-sdk/meta-alp-sdk
```
- [ ] **Step 2: Write the failing test (`test_bsp_manifest.py`):**
```python
from alp_cli.bsp.manifest import load_bsp_manifest

def test_loads_renesas_manifest():
    m = load_bsp_manifest("renesas-rzv2n")
    assert m.family == "renesas-rzv2n"
    assert m.package_id == "RTK0EF0045Z94001AZJ"
    assert "alp-sdk/meta-alp-sdk" in m.layers
    assert m.machine_pattern.format(sku="v2n101") == "e1m-v2n101-a55"
```
- [ ] **Step 3: Run → FAIL** (`pytest tests/scripts/alp_cli/test_bsp_manifest.py -v`).
- [ ] **Step 4: Implement `manifest.py`:**
```python
from dataclasses import dataclass
from pathlib import Path
import yaml

_BSP_DIR = Path(__file__).resolve().parents[3] / "metadata" / "bsp"

@dataclass(frozen=True)
class BspManifest:
    family: str
    package_id: str
    version: str
    templateconf: str
    machine_pattern: str
    layers: list[str]

def load_bsp_manifest(family: str) -> BspManifest:
    d = yaml.safe_load((_BSP_DIR / f"{family}.yaml").read_text(encoding="utf-8"))
    return BspManifest(d["family"], d["bsp"]["package_id"], d["bsp"]["version"],
                       d["templateconf"], d["machine_pattern"], list(d["layers"]))
```
- [ ] **Step 5: Run → PASS. Commit.**

---

### Task 2 🖥️ — BSP locator

**Files:** Create `scripts/alp_cli/bsp/locate.py`; Test `test_bsp_locate.py`

- [ ] **Step 1: Failing test:**
```python
import pytest
from alp_cli.bsp.locate import locate_bsp, BspNotFound

def test_locate_from_env(tmp_path, monkeypatch):
    monkeypatch.setenv("ALP_RZV2N_BSP", str(tmp_path))
    assert locate_bsp("renesas-rzv2n") == tmp_path

def test_missing_bsp_raises_actionable(monkeypatch):
    monkeypatch.delenv("ALP_RZV2N_BSP", raising=False)
    with pytest.raises(BspNotFound) as e:
        locate_bsp("renesas-rzv2n", cli_path=None, config={})
    assert "RTK0EF0045Z94001AZJ" in str(e.value)   # tells the user what to download
```
- [ ] **Step 2: Run → FAIL.**
- [ ] **Step 3: Implement `locate.py`** (precedence: `--bsp` > env > `~/.alp/bsp.toml`; missing → actionable error naming the package):
```python
import os
from pathlib import Path

_ENV = {"renesas-rzv2n": "ALP_RZV2N_BSP", "nxp-imx93": "ALP_IMX93_BSP"}
_PKG = {"renesas-rzv2n": "RTK0EF0045Z94001AZJ-v1.0.3.zip (My Renesas, free signup)",
        "nxp-imx93": "the NXP meta-imx release matching your board"}

class BspNotFound(RuntimeError):
    pass

def locate_bsp(family: str, cli_path: str | None = None, config: dict | None = None) -> Path:
    config = config or {}
    for cand in (cli_path, os.environ.get(_ENV.get(family, "")), config.get(family)):
        if cand and Path(cand).is_dir():
            return Path(cand)
    raise BspNotFound(
        f"No BSP for '{family}'. Download {_PKG.get(family, family)}, extract it, "
        f"and point {_ENV.get(family, 'the BSP path')} at it (or pass --bsp / set "
        f"it in ~/.alp/bsp.toml).")
```
- [ ] **Step 4: Run → PASS. Commit.**

---

### Task 3 🖥️ — `BspProvider` base (manifest-driven) + registry

**Files:** Create `scripts/alp_cli/bsp/base.py`; Test `test_bsp_registry.py`

- [ ] **Step 1: Failing test:**
```python
import pytest
from alp_cli.bsp.base import get_provider, BspProvider

def test_registry_returns_family_provider():
    p = get_provider("renesas-rzv2n")
    assert isinstance(p, BspProvider) and p.family == "renesas-rzv2n"

def test_unknown_family_raises():
    with pytest.raises(KeyError):
        get_provider("totally-unknown")
```
- [ ] **Step 2: Run → FAIL.**
- [ ] **Step 3: Implement `base.py`** (generic, manifest-driven; subclasses override only quirks):
```python
from __future__ import annotations
import subprocess
from dataclasses import dataclass
from pathlib import Path
from .manifest import BspManifest, load_bsp_manifest

@dataclass
class Artifacts:
    image: Path
    extra: list[Path]

class BspProvider:
    family: str = ""

    def __init__(self, runner=subprocess.run):
        self._run = runner
        self.manifest: BspManifest = load_bsp_manifest(self.family)

    def validate(self, bsp_root: Path) -> None:
        if not (bsp_root / "poky").is_dir():
            raise FileNotFoundError(f"{bsp_root} is not an extracted {self.family} BSP (no poky/).")

    def prepare(self, bsp_root: Path) -> Path:
        """Apply TEMPLATECONF + add the manifest's layers. Returns the build dir."""
        build = bsp_root / "build"
        for layer in self.manifest.layers:
            self._run(["bitbake-layers", "add-layer", str(bsp_root / layer)], cwd=build, check=True)
        return build

    def bake(self, build: Path, machine: str, image: str) -> Artifacts:
        self._run(["bitbake", image], cwd=build, check=True,
                  env={"MACHINE": machine})
        out = build / "tmp" / "deploy" / "images" / machine
        return Artifacts(image=out / f"{image}-{machine}.wic", extra=[out / "Image"])

_REGISTRY: dict[str, type[BspProvider]] = {}

def register(cls): _REGISTRY[cls.family] = cls; return cls
def get_provider(family: str, runner=subprocess.run) -> BspProvider:
    return _REGISTRY[family](runner=runner)
```
- [ ] **Step 4: Run → FAIL still** (no providers registered yet — that's Task 4). Mark this test `xfail` until Task 4, or register a stub. **Commit.**

---

### Task 4 🖥️ — Renesas + NXP providers

**Files:** Create `scripts/alp_cli/bsp/renesas_rzv2n.py`, `nxp_imx93.py`, `metadata/bsp/nxp-imx93.yaml`; Test `test_renesas_provider.py`

- [ ] **Step 1: Failing test (mock subprocess; assert the right invocations, no real bitbake):**
```python
from pathlib import Path
from unittest.mock import MagicMock
from alp_cli.bsp.base import get_provider

def test_renesas_prepare_adds_meta_alp_sdk(tmp_path):
    (tmp_path / "poky").mkdir()
    runner = MagicMock()
    p = get_provider("renesas-rzv2n", runner=runner)
    p.validate(tmp_path)
    p.prepare(tmp_path)
    calls = [c.args[0] for c in runner.call_args_list]
    assert ["bitbake-layers", "add-layer", str(tmp_path / "alp-sdk/meta-alp-sdk")] in calls

def test_renesas_bake_invokes_bitbake(tmp_path):
    runner = MagicMock()
    p = get_provider("renesas-rzv2n", runner=runner)
    p.bake(tmp_path / "build", "e1m-v2n101-a55", "alp-image-edge")
    assert runner.call_args_list[-1].args[0] == ["bitbake", "alp-image-edge"]
```
- [ ] **Step 2: Run → FAIL.**
- [ ] **Step 3: Implement `renesas_rzv2n.py`** (thin subclass; the README's TEMPLATECONF tarball flow is the only quirk):
```python
from .base import BspProvider, register

@register
class RenesasRzv2nProvider(BspProvider):
    family = "renesas-rzv2n"
    # Generic prepare()/bake() from the manifest suffice; override prepare()
    # only to source oe-init-build-env with the manifest's TEMPLATECONF before
    # adding layers (Phase-1-proven flow).
```
- [ ] **Step 4: Implement `nxp_imx93.py` + `metadata/bsp/nxp-imx93.yaml`** (meta-imx/meta-freescale layer set; same generic engine):
```python
from .base import BspProvider, register

@register
class NxpImx93Provider(BspProvider):
    family = "nxp-imx93"
```
```yaml
# metadata/bsp/nxp-imx93.yaml
family: nxp-imx93
bsp: { package_id: meta-imx, version: scarthgap, ai_sdk: "n/a", yocto: scarthgap-5.0.11 }
templateconf: sources/meta-imx/meta-imx-bsp/conf/templates/default
machine_pattern: "e1m-{sku}-a55"
layers: [ meta-freescale, meta-imx/meta-imx-bsp, alp-sdk/meta-alp-sdk ]
```
- [ ] **Step 5:** Ensure `scripts/alp_cli/bsp/__init__.py` imports both providers so `@register` runs. **Run → PASS. Commit.**

---

### Task 5 🖥️ — Dispatcher

**Files:** Create `scripts/alp_cli/build.py` (dispatcher part); Test `test_dispatch.py`

- [ ] **Step 1: Failing test** (a `board.yaml` fixture with an A55 yocto slice + an M33 zephyr slice → assert routing):
```python
from unittest.mock import MagicMock
from alp_cli.build import dispatch

def test_dispatch_routes_by_os(tmp_path, monkeypatch):
    project = MagicMock()
    project.cores = {
        "a55_cluster": MagicMock(os="yocto", machine="e1m-v2n101-a55", image="alp-image-edge", core_id="a55_cluster"),
        "m33_sm":      MagicMock(os="zephyr", core_id="m33_sm"),
    }
    project.som_family = "renesas-rzv2n"
    zephyr, yocto = MagicMock(), MagicMock()
    dispatch(project, zephyr_builder=zephyr, yocto_runner=yocto)
    zephyr.build.assert_called_once()
    yocto.assert_called_once()   # provider bake path invoked for the a55 slice
```
- [ ] **Step 2: Run → FAIL.**
- [ ] **Step 3: Implement `dispatch()` in `build.py`:**
```python
from .bsp.base import get_provider
from .bsp.locate import locate_bsp

def dispatch(project, zephyr_builder, yocto_runner=None):
    results = []
    for core_id, sl in project.cores.items():
        if sl.os == "off":
            continue
        if sl.os == "zephyr":
            results.append(zephyr_builder.build(sl))
        elif sl.os == "yocto":
            family = project.som_family
            provider = get_provider(family)
            bsp = locate_bsp(family)
            provider.validate(bsp)
            build = provider.prepare(bsp)
            results.append((yocto_runner or provider.bake)(build, sl.machine, sl.image))
    return results
```
*(Note: confirm `project.som_family` in Task 0 — if the resolved `BoardProject` exposes the family differently, adapt the lookup.)*
- [ ] **Step 4: Run → PASS. Commit.**

---

### Task 6 🖥️ — `alp build` click command + register

**Files:** Modify `scripts/alp_cli/build.py` (add the command), `scripts/alp_cli/main.py`; Test `test_build_cli.py`

- [ ] **Step 1: Failing test (click CliRunner, mocked builders):**
```python
from click.testing import CliRunner
from unittest.mock import patch
from alp_cli.main import cli

def test_build_reports_artifacts(tmp_path):
    (tmp_path / "board.yaml").write_text("som: {sku: E1M-V2N101}\ncores: {a55_cluster: {}}\n")
    with patch("alp_cli.build.dispatch", return_value=["<artifacts>"]):
        r = CliRunner().invoke(cli, ["build", "-C", str(tmp_path)])
    assert r.exit_code == 0 and "artifacts" in r.output.lower()
```
- [ ] **Step 2: Run → FAIL.**
- [ ] **Step 3: Add the command in `build.py`:**
```python
import click
from pathlib import Path
from alp_orchestrate import load_board_yaml
from .zephyr_build import ZephyrBuilder

@click.command(name="build", help="Build all cores (Zephyr + Linux) from board.yaml.")
@click.option("-C", "project_dir", default=".", type=click.Path(file_okay=False))
@click.option("--core", default=None, help="Build only this core id.")
def build_cmd(project_dir, core):
    project = load_board_yaml(Path(project_dir) / "board.yaml")
    arts = dispatch(project, zephyr_builder=ZephyrBuilder())
    click.echo(f"Built artifacts: {arts}")
```
- [ ] **Step 4: Register in `main.py`:** `from .build import build_cmd` then `cli.add_command(build_cmd)`.
- [ ] **Step 5: Implement `zephyr_build.py`** (`ZephyrBuilder.build(slice)` wraps `west build` — reuse `alp run`'s existing west invocation in `run.py`).
- [ ] **Step 6: Run → PASS. Commit.**

---

### Task 7 🖥️ — Reproducibility-drift check

- [ ] **Step 1: Failing test:** `provider.validate()` warns/raises when the BSP's actual version ≠ `manifest.version` (add a `VERSION` marker read in `validate`; `--strict` raises, else warns).
- [ ] **Step 2: Implement** the version compare in `BspProvider.validate` (read the BSP's version file; compare to `self.manifest.version`).
- [ ] **Step 3: Run → PASS. Commit.**

---

### Task 8 🔧 — Integration: real `alp build` bake (bench)

- [ ] **Step 1:** On the bench (post Phase-1), `ALP_RZV2N_BSP=<extracted> alp build -C examples/<v2n-project>` → drives the Renesas provider + bakes `alp-image-edge`; verify the same `.wic`/`Image`/dtb as Phase-1's manual bake.
- [ ] **Step 2:** Repeat for `e1m-nx9101-a55` (NXP provider) once an i.MX93 BSP is available.

**Phase 2 exit:** `alp build` (any supported SoM) builds Zephyr + Yocto from `board.yaml`; V2N + i.MX93 green; reproducibility checked.

---

## Self-review notes
- Spec coverage: §4 architecture → Tasks 3/5/6; §5 components → Tasks 1–6; §8 error handling → Tasks 2 (locator msg) + 7 (drift) + provider `validate`; §9 testing → unit Tasks 1–7, integration Task 8. ✓
- Open dependency: `project.som_family` exact attribute (Task 0 confirms; `BoardProject` is a dataclass in `alp_orchestrate.py` — adapt if the family lives on `project.som.family` or similar).
- TDD-shape note: Task 3's registry test stays red until Task 4 registers providers — sequence Tasks 3→4 together or `xfail` the registry test in Task 3.
