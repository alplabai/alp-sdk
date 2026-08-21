"""`alp doctor` -- read-only environment preflight for the Alp SDK.

Runs a battery of host / filesystem / `--version` checks and prints one
`[PASS]`/`[WARN]`/`[FAIL]` line each with a remediation hint, mirroring the
truth encoded in `scripts/bootstrap.sh` (workspace venv at
`<topdir>/.venv` beside alp-sdk, the Zephyr pin read live from west.yml,
`.west`/`VERSION` probing).

It is strictly HW-free: no build, no board, no flash -- pure environment,
filesystem and `--version` inspection -- so it is safe to run anywhere,
anytime, before you ever touch silicon.

Scope: the HOST and the WORKSPACE only.  Verdicts about a customer's own
project -- notably the ADR 0018 curated-library selection (tier, licence and
whether each selection can be wired on the target) -- belong to `tan doctor`,
which is the user command surface under ADR 0020 end-state B.  This module is
alp-sdk's internal/reference preflight, not a second user CLI.

Exit code: 0 when no check FAILs, 1 when any check FAILs.  `--strict`
promotes WARN to a nonzero exit too; `--json` emits a machine-readable
report for the VS Code extension.
"""

from __future__ import annotations

import importlib.util
import json as _json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import click

# Reuse the validator's colour policy (honours NO_COLOR / non-tty) instead of
# reinventing it.  The Fore/Style fallback mirrors diagnostic.py so the command
# still runs if colorama is somehow absent.
from alp_cli.diagnostic import _use_color

try:
    from colorama import Fore, Style
except ImportError:  # pragma: no cover - colorama is a hard dependency
    class _Stub:
        def __getattr__(self, _: str) -> str:
            return ""

    Fore = _Stub()  # type: ignore[assignment]
    Style = _Stub()  # type: ignore[assignment]


# -------- result type ---------------------------------------------------------

PASS = "PASS"
WARN = "WARN"
FAIL = "FAIL"

# Fallback when the SDK's west.yml can't be read; the live pin comes from
# _zephyr_pin() so a west.yml bump never leaves doctor checking a stale pin.
ZEPHYR_PIN = "v4.4.0"
ZEPHYR_SDK_VERSION = "zephyr-sdk-1.0.1"

_REQUIRED_DEPS = ("yaml", "jsonschema", "click", "cbor2", "questionary", "colorama")


@dataclass(slots=True)
class CheckResult:
    name: str
    status: str  # PASS | WARN | FAIL
    message: str
    hint: str | None = None


# -------- small probing helpers ----------------------------------------------


def _parse_two(text: str) -> tuple[int, int] | None:
    """Pull the first ``MAJOR.MINOR`` pair out of arbitrary version text."""
    m = re.search(r"(\d+)\.(\d+)", text)
    return (int(m.group(1)), int(m.group(2))) if m else None


def _tool_version(args: list[str]) -> tuple[int, int] | None:
    """Run ``args`` and parse a ``(major, minor)`` from stdout+stderr.

    Returns ``None`` if the tool is missing or never prints a version.  Split
    out as its own function so tests can monkeypatch it without shelling out.
    """
    try:
        proc = subprocess.run(
            args, capture_output=True, text=True, timeout=10, check=False
        )
    except (OSError, subprocess.SubprocessError):  # pragma: no cover - env-dependent
        return None
    return _parse_two((proc.stdout or "") + "\n" + (proc.stderr or ""))


def _repo_root() -> Path:
    # scripts/alp_cli/doctor.py -> repo root is two parents up from the package.
    return Path(__file__).resolve().parents[2]


def _workspace_dir() -> Path:
    """The west workspace topdir bootstrap.sh creates: the alp-sdk checkout's
    parent (bootstrap does `west init -l <alp-sdk>`, so alp-sdk is the manifest
    repo and its parent is the topdir; #769)."""
    return _repo_root().parent


def _zephyr_pin() -> str:
    """The Zephyr `revision:` pin, read live from the SDK's west.yml.

    Falls back to ZEPHYR_PIN when west.yml is missing/unparseable (e.g. a
    packaged install without the repo checkout).
    """
    try:
        text = (_repo_root() / "west.yml").read_text(encoding="utf-8")
    except OSError:
        return ZEPHYR_PIN
    m = re.search(r"-\s+name:\s+zephyr\s*\n\s+revision:\s+(\S+)", text)
    return m.group(1) if m else ZEPHYR_PIN


def _pin_mm() -> tuple[int, int]:
    """(MAJOR, MINOR) of the pinned Zephyr version."""
    return _parse_two(_zephyr_pin()) or (4, 4)


def _bootstrap_manifest() -> dict | None:
    """Load metadata/bootstrap.json -- the single source (issue #949) for the
    per-tool prerequisite install command every check below used to hardcode
    its own (drifted) copy of. Returns None on any read/parse problem --
    including a file that isn't valid UTF-8, or one that parses but whose
    top level isn't a JSON object -- so a check can fall back to a generic
    hint instead of crashing `alp doctor` (a packaged install without the
    repo checkout has no metadata/ at all; a truncated or bad-merge
    bootstrap.json is exactly the broken workspace this command exists to
    diagnose)."""
    try:
        text = (_repo_root() / "metadata" / "bootstrap.json").read_text(encoding="utf-8")
        data = _json.loads(text)
    except (OSError, ValueError):  # ValueError covers JSONDecodeError + UnicodeDecodeError
        return None
    return data if isinstance(data, dict) else None


def _prereq_os_key() -> str:
    if _is_windows():
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    return "linux"


def _prereq_linux_pm() -> str | None:
    """Which package manager's sub-map to read under
    prerequisites.install.linux (issue #1464 -- keyed by PACKAGE MANAGER,
    not distro, since a `sudo apt-get install`-shaped command is wrong on a
    host with no apt at all). `command -v`-style detection via
    `shutil.which`, checked in the same apt-before-dnf order
    scripts/bootstrap.sh's own PM detection uses. Returns None when neither
    resolves (or on a PM this manifest deliberately ships no sub-map for,
    e.g. Arch's pacman) -- the caller then falls back to the generic,
    package-manager-agnostic hint rather than guessing."""
    if shutil.which("apt-get") is not None:
        return "apt"
    if shutil.which("dnf") is not None:
        return "dnf"
    return None


def _prereq_install_hint(tool: str) -> str:
    """The remediation string for a missing prerequisite `tool`, sourced from
    metadata/bootstrap.json's prerequisites.install.<os> map -- the same
    manifest scripts/bootstrap.sh and scripts/bootstrap.ps1 read (issue
    #949). On Linux, `install.linux` is itself keyed by PACKAGE MANAGER
    (issue #1464 -- `install.linux.apt` / `install.linux.dnf`), so this adds
    one extra hop (`_prereq_linux_pm()`) between `install` and the OS key on
    that path only; macOS/Windows are unaffected (each has exactly one
    relevant package manager, so their maps stay flat). Falls back to a
    generic, package-manager-agnostic pointer when the manifest is
    unreadable, when no known package manager is detected on a Linux host,
    when `tool` isn't tracked as an install command for the resolved
    OS/(package manager) at all (e.g. `ninja` under `install.linux.dnf` --
    unshipped because this manifest could not verify one dnf-family command
    that works on both Fedora and RHEL-derivatives without EPEL; printing an
    invented command here would just reintroduce the drift this change
    removes), or when the manifest parses but is shaped wrong at any hop --
    each hop is guarded with `isinstance(node, dict)` before it is indexed,
    so a truncated/bad-merge manifest degrades to the generic hint instead
    of raising."""
    node: object = _bootstrap_manifest()
    os_key = _prereq_os_key()
    keys = ["prerequisites", "install", os_key]
    if os_key == "linux":
        pm = _prereq_linux_pm()
        if pm is None:
            return f"Install {tool} via your OS package manager."
        keys.append(pm)
    keys.append(tool)
    for key in keys:
        if not isinstance(node, dict):
            node = None
            break
        node = node.get(key)
    if isinstance(node, str) and node:
        return f"Install it: {node}."
    return f"Install {tool} via your OS package manager."


def _python_pin() -> str | None:
    """The dev/CI interpreter pin, read live from the repo's .python-version.

    Returns ``None`` when the file is absent or empty (e.g. a packaged
    install without the repo checkout) so the check can skip instead of
    guessing.
    """
    try:
        text = (_repo_root() / ".python-version").read_text(encoding="utf-8")
    except OSError:
        return None
    for line in text.splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            return line
    return None


# -------- individual checks ---------------------------------------------------


def _check_python() -> CheckResult:
    v = sys.version_info
    cur = f"{v.major}.{v.minor}.{v.micro}"
    if (v.major, v.minor) >= (3, 10):
        return CheckResult("python", PASS, f"Python {cur}")
    return CheckResult(
        "python", FAIL, f"Python {cur} is below the required 3.10",
        "Install Python 3.10+ (pyproject requires-python = \">=3.10\").",
    )


def _check_python_pin() -> CheckResult | None:
    # Dev/CI standardise on the interpreter pinned in .python-version (the
    # single source every actions/setup-python reads).  requires-python in
    # pyproject.toml stays the SUPPORT floor (>= 3.10); this check only
    # nudges towards the pinned version, so a mismatch is WARN, never FAIL.
    pin = _python_pin()
    if pin is None:
        return None  # packaged install without the repo checkout -- skip
    v = sys.version_info
    cur = f"{v.major}.{v.minor}.{v.micro}"
    pin_mm = _parse_two(pin)
    if pin_mm is None:
        return CheckResult(
            "python-pin", WARN, f".python-version pin {pin!r} is unparseable",
            "Fix .python-version to a MAJOR.MINOR like 3.12.",
        )
    if (v.major, v.minor) == pin_mm:
        return CheckResult("python-pin", PASS, f"Python {cur} matches the {pin} pin")
    return CheckResult(
        "python-pin", WARN,
        f"Python {cur} != pinned {pin} (.python-version)",
        f"Dev/CI standardise on Python {pin}; >= 3.10 still works "
        "(pyproject support floor) but CI runs the pinned version.",
    )


def _check_west() -> CheckResult:
    if shutil.which("west") is None:
        return CheckResult(
            "west", FAIL, "west not found on PATH",
            "Install it into the workspace venv: pip install west.",
        )
    ver = _tool_version(["west", "--version"])
    if ver is None:
        return CheckResult(
            "west", WARN, "west present but its version could not be parsed",
            "Run `west --version` manually; expected >= 1.2.",
        )
    if ver >= (1, 2):
        return CheckResult("west", PASS, f"west {ver[0]}.{ver[1]}")
    return CheckResult(
        "west", WARN, f"west {ver[0]}.{ver[1]} is older than 1.2",
        "Upgrade: pip install --upgrade west.",
    )


def _check_python_deps() -> CheckResult:
    missing = [m for m in _REQUIRED_DEPS if importlib.util.find_spec(m) is None]
    if not missing:
        return CheckResult("python-deps", PASS, "all required Python deps importable")
    return CheckResult(
        "python-deps", FAIL, f"missing Python deps: {', '.join(missing)}",
        "pip install pyyaml jsonschema click cbor2 questionary colorama "
        "(or run scripts/bootstrap.sh).",
    )


def _check_cmake() -> CheckResult:
    # cmake is tracked in both prerequisites.posix and prerequisites.windows,
    # so routing its two install/upgrade hints through _prereq_install_hint
    # exercises all three prerequisites.install OS maps (linux/macos/windows)
    # at run time (ninja, below, now covers the same three since #971).
    if shutil.which("cmake") is None:
        return CheckResult(
            "cmake", FAIL, "cmake not found on PATH", _prereq_install_hint("cmake"),
        )
    ver = _tool_version(["cmake", "--version"])
    if ver is None:
        return CheckResult(
            "cmake", FAIL, "cmake present but its version could not be parsed",
            "Run `cmake --version` manually; expected >= 3.20.",
        )
    if ver >= (3, 20):
        return CheckResult("cmake", PASS, f"cmake {ver[0]}.{ver[1]}")
    return CheckResult(
        "cmake", FAIL, f"cmake {ver[0]}.{ver[1]} is below the required 3.20",
        _prereq_install_hint("cmake"),
    )


def _check_ninja() -> CheckResult:
    # Zephyr's default CMake generator; every west build needs it. The
    # remediation hint is sourced from metadata/bootstrap.json (issue #949)
    # instead of a hardcoded per-OS string -- this used to hand out a
    # winget ID missing `-e --id` and an apt package name matching no
    # canonical command anywhere else.
    if shutil.which("ninja") is None:
        return CheckResult(
            "ninja", FAIL, "ninja not found on PATH", _prereq_install_hint("ninja"),
        )
    ver = _tool_version(["ninja", "--version"])
    label = f"ninja {ver[0]}.{ver[1]}" if ver else "ninja present"
    return CheckResult("ninja", PASS, label)


def _check_dtc() -> CheckResult:
    # The devicetree compiler.  Zephyr's build runs it for extra dts
    # validation when present; WARN-only because edtlib does the
    # load-bearing parse in pure Python.
    if shutil.which("dtc") is None:
        return CheckResult(
            "dtc", WARN, "devicetree compiler (dtc) not found on PATH",
            "Install it: apt install device-tree-compiler / brew install dtc "
            "-- on native Windows the Zephyr SDK bundle does not ship dtc, "
            "so it needs a separate install (see docs/cross-platform-setup.md "
            "for Windows steps).",
        )
    ver = _tool_version(["dtc", "--version"])
    label = f"dtc {ver[0]}.{ver[1]}" if ver else "dtc present"
    return CheckResult("dtc", PASS, label)


def _check_gperf() -> CheckResult:
    # Needed by Zephyr's kobject/userspace generation; WARN-only because
    # plain kernel-mode apps build without it.
    if shutil.which("gperf") is None:
        return CheckResult(
            "gperf", WARN, "gperf not found on PATH",
            "Install it: apt install gperf / brew install gperf "
            "-- on native Windows the Zephyr SDK bundle does not ship gperf "
            "either, so it needs a separate install (see "
            "docs/cross-platform-setup.md for Windows steps).",
        )
    ver = _tool_version(["gperf", "--version"])
    label = f"gperf {ver[0]}.{ver[1]}" if ver else "gperf present"
    return CheckResult("gperf", PASS, label)


def _check_imgtool() -> CheckResult:
    # MCUboot image signing (secure-boot flows).  WARN-only: unsigned
    # bring-up builds don't need it.
    if importlib.util.find_spec("imgtool") is not None or \
            shutil.which("imgtool") is not None:
        return CheckResult("imgtool", PASS, "imgtool available")
    return CheckResult(
        "imgtool", WARN, "imgtool not importable / not on PATH",
        "pip install imgtool (needed to sign MCUboot images; "
        "not needed for unsigned bring-up builds).",
    )


def _check_jlink() -> CheckResult:
    # Optional probe tooling: only SWD flash/debug flows need it.
    for exe in ("JLinkExe", "JLink"):
        found = shutil.which(exe)
        if found:
            return CheckResult("jlink", PASS, f"SEGGER J-Link tools ({found})")
    return CheckResult(
        "jlink", WARN, "SEGGER J-Link tools not on PATH (optional)",
        "Install J-Link Software & Documentation Pack if you flash/debug "
        "over SWD; not needed for native_sim or bootloader-based flashing.",
    )


def _check_tan() -> CheckResult:
    # Tan is the standalone Python planner/executor (ADR 0020). SDK-reference
    # emit/validation work may intentionally run without it. WARN-only.
    found = shutil.which("tan")
    if found is None:
        return CheckResult(
            "tan", WARN, "tan (build executor) not found on PATH",
            "See docs/cli.md: use tan-cli/dev during the v0.5 port, then the "
            "release installer from v0.5 onward (not needed for direct SDK "
            "reference emit/validation work).",
        )
    ver = _tool_version(["tan", "--version"])
    label = f"tan {ver[0]}.{ver[1]} ({found})" if ver else f"tan present ({found})"
    return CheckResult("tan", PASS, label)


def _check_host_compiler() -> CheckResult:
    # native_sim builds with host gcc (>=11) or clang (>=14).  Missing is only a
    # WARN -- real-silicon work uses the Zephyr SDK cross-toolchain instead.
    if shutil.which("gcc") is not None:
        ver = _tool_version(["gcc", "-dumpfullversion", "-dumpversion"])
        if ver and ver >= (11, 0):
            return CheckResult("host-compiler", PASS, f"gcc {ver[0]}.{ver[1]}")
        if ver:
            return CheckResult(
                "host-compiler", WARN, f"gcc {ver[0]}.{ver[1]} is older than 11",
                "Install gcc >= 11 (or clang >= 14) for native_sim builds.",
            )
    if shutil.which("clang") is not None:
        ver = _tool_version(["clang", "--version"])
        if ver and ver >= (14, 0):
            return CheckResult("host-compiler", PASS, f"clang {ver[0]}.{ver[1]}")
        if ver:
            return CheckResult(
                "host-compiler", WARN, f"clang {ver[0]}.{ver[1]} is older than 14",
                "Install clang >= 14 (or gcc >= 11) for native_sim builds.",
            )
    return CheckResult(
        "host-compiler", WARN, "no host C compiler (gcc/clang) found",
        "Install gcc >= 11 or clang >= 14 for native_sim builds.",
    )


def _zephyr_base() -> Path | None:
    raw = os.environ.get("ZEPHYR_BASE")
    return Path(raw) if raw else None


def _check_zephyr_base() -> CheckResult:
    base = _zephyr_base()
    if base is None:
        return CheckResult(
            "zephyr-base", WARN, "ZEPHYR_BASE is not set",
            "export ZEPHYR_BASE=<workspace>/zephyr "
            "(see scripts/bootstrap.sh --print-env).",
        )
    if (base / "VERSION").is_file():
        return CheckResult("zephyr-base", PASS, f"ZEPHYR_BASE -> {base}")
    return CheckResult(
        "zephyr-base", FAIL,
        f"ZEPHYR_BASE ({base}) has no VERSION file -- not a Zephyr tree",
        "Point ZEPHYR_BASE at the zephyr/ checkout produced by west init.",
    )


def _read_zephyr_mm(base: Path) -> tuple[int, int] | None:
    version_file = base / "VERSION"
    if not version_file.is_file():
        return None
    major = minor = None
    try:
        text = version_file.read_text(encoding="utf-8")
    except OSError:  # pragma: no cover - env-dependent
        return None
    for line in text.splitlines():
        if line.startswith("VERSION_MAJOR"):
            m = re.search(r"(\d+)", line)
            if m:
                major = int(m.group(1))
        elif line.startswith("VERSION_MINOR"):
            m = re.search(r"(\d+)", line)
            if m:
                minor = int(m.group(1))
    if major is None or minor is None:
        return None
    return (major, minor)


def _check_zephyr_version() -> CheckResult:
    pin = _zephyr_pin()
    pin_mm = _pin_mm()
    base = _zephyr_base()
    if base is None or not (base / "VERSION").is_file():
        return CheckResult(
            "zephyr-version", WARN, "cannot verify Zephyr version (no ZEPHYR_BASE tree)",
            f"Set ZEPHYR_BASE to a Zephyr {pin} tree.",
        )
    mm = _read_zephyr_mm(base)
    if mm is None:
        return CheckResult(
            "zephyr-version", WARN, "could not parse $ZEPHYR_BASE/VERSION",
            f"Expected MAJOR.MINOR == {pin_mm[0]}.{pin_mm[1]} ({pin}).",
        )
    if mm == pin_mm:
        return CheckResult("zephyr-version", PASS, f"Zephyr {mm[0]}.{mm[1]}.x (pin {pin})")
    return CheckResult(
        "zephyr-version", FAIL,
        f"Zephyr {mm[0]}.{mm[1]}.x != pinned {pin_mm[0]}.{pin_mm[1]}.x",
        f"Stale Zephyr tree vs the pinned {pin} (west.yml): run `west update`.",
    )


def _manifest_dir(topdir: Path) -> Path | None:
    """Resolve the workspace's manifest-repo directory from `.west/config`.

    Read directly rather than shelling out to `west config`: doctor must work
    even when the workspace is broken enough that west itself misbehaves.
    """
    cfg = topdir / ".west" / "config"
    try:
        text = cfg.read_text(encoding="utf-8")
    except OSError:
        return None
    # The [manifest] section's `path` is relative to topdir.
    in_manifest = False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            in_manifest = stripped[1:-1].strip() == "manifest"
            continue
        if not in_manifest:
            continue
        m = re.match(r"^path\s*=\s*(.+?)\s*$", stripped)
        if m:
            try:
                return (topdir / m.group(1)).resolve()
            except OSError:
                return None
    return None


def _check_west_workspace() -> CheckResult:
    base = _zephyr_base()
    if base is None:
        return CheckResult(
            "west-workspace", WARN, "cannot check .west (ZEPHYR_BASE unset)",
            "Set ZEPHYR_BASE, then ensure <workspace>/.west exists (west init).",
        )
    topdir = base.parent
    if not (topdir / ".west").is_dir():
        return CheckResult(
            "west-workspace", FAIL,
            f"no .west directory beside ZEPHYR_BASE ({topdir})",
            "Initialise the workspace: run scripts/bootstrap.sh (or west init).",
        )
    # A .west directory is necessary but NOT sufficient: if the workspace's
    # manifest repo isn't alp-sdk, west never discovers alp-sdk's
    # `self.west-commands`, so `west alp-migrate` (and alp-lock/-quality/-emit)
    # stay "unknown command" (issue #769).  Checking only for `.west` reports a
    # healthy workspace on exactly the layout #769 was filed about.
    manifest = _manifest_dir(topdir)
    repo = _repo_root().resolve()
    if manifest is None:
        return CheckResult(
            "west-workspace", WARN,
            f"west workspace at {topdir}, but its manifest path could not be read",
            f"Check '{topdir / '.west' / 'config'}' has a [manifest] path entry.",
        )
    if manifest != repo:
        return CheckResult(
            "west-workspace", FAIL,
            f"west workspace at {topdir} has manifest '{manifest}', not alp-sdk ({repo}) "
            "-- 'west alp-migrate' will be an unknown command (#769)",
            "Re-bootstrap so alp-sdk is the manifest repo: scripts/bootstrap.sh "
            "(or scripts/bootstrap.ps1 on native Windows), which runs "
            "'west init -l <alp-sdk>'. An existing plain-Zephyr workspace is not reused.",
        )
    return CheckResult(
        "west-workspace", PASS,
        f"west workspace at {topdir} (manifest: alp-sdk -- alp-* commands register)",
    )


def _check_workspace_venv() -> CheckResult:
    # Prefer the venv beside the active ZEPHYR_BASE workspace; fall back to the
    # canonical <topdir>/.venv that bootstrap.sh creates (topdir = alp-sdk's
    # parent, #769).
    candidates: list[Path] = []
    base = _zephyr_base()
    if base is not None:
        candidates.append(base.parent / ".venv")
    candidates.append(_workspace_dir() / ".venv")

    venv = next((c for c in candidates if c.is_dir()), None)
    if venv is None:
        return CheckResult(
            "workspace-venv", WARN, "workspace venv (<topdir>/.venv beside alp-sdk) not found",
            "Create it with scripts/bootstrap.sh.",
        )
    # Active if the running interpreter lives inside the venv.
    try:
        active = Path(sys.prefix).resolve() == venv.resolve()
    except OSError:  # pragma: no cover - env-dependent
        active = False
    if active:
        return CheckResult("workspace-venv", PASS, f"workspace venv active ({venv})")
    return CheckResult(
        "workspace-venv", WARN, f"workspace venv present but not active ({venv})",
        f"Activate it: source {venv}/bin/activate.",
    )


def _check_hal_alif() -> CheckResult:
    # AEN (Alif Ensemble) targets need the hal_alif Zephyr module AND alp-sdk to
    # define a matching USE_ALIF_HAL_<X>.  Soft, AEN-only WARN -- a missing
    # hal_alif is fine for V2N / native_sim work, so keep detection lightweight.
    ws = _workspace_dir()
    hal_present = any(
        (ws / "modules" / sub).is_dir()
        for sub in ("hal/alif", "hal_alif", "hal/hal_alif")
    )
    use_alif_defined = False
    repo = _repo_root()
    try:
        # A cheap grep: any USE_ALIF_HAL_* token anywhere under the SDK tree.
        for cfg in repo.glob("**/*.cmake"):
            if "USE_ALIF_HAL_" in cfg.read_text(encoding="utf-8", errors="ignore"):
                use_alif_defined = True
                break
    except OSError:  # pragma: no cover - env-dependent
        pass
    if hal_present and use_alif_defined:
        return CheckResult("hal-alif", PASS, "hal_alif module present + USE_ALIF_HAL_* defined")
    return CheckResult(
        "hal-alif", WARN,
        "hal_alif module / USE_ALIF_HAL_* not detected (AEN targets only)",
        "Add hal_alif to the west workspace and define USE_ALIF_HAL_<X> "
        "(not needed for V2N / native_sim).",
    )


def _check_zephyr_sdk() -> CheckResult:
    # Real-silicon cross builds need the Zephyr SDK; native_sim does not.
    env_dir = os.environ.get("ZEPHYR_SDK_INSTALL_DIR")
    candidates: list[Path] = []
    if env_dir:
        candidates.append(Path(env_dir))
    home = Path.home()
    candidates += [
        home / ZEPHYR_SDK_VERSION,
        home / ".local" / "opt" / ZEPHYR_SDK_VERSION,
        Path("/opt") / ZEPHYR_SDK_VERSION,
        Path("/usr/local") / ZEPHYR_SDK_VERSION,
    ]
    found = next((c for c in candidates if c.is_dir()), None)
    if found is not None:
        return CheckResult("zephyr-sdk", PASS, f"{ZEPHYR_SDK_VERSION} at {found}")
    return CheckResult(
        "zephyr-sdk", WARN, f"{ZEPHYR_SDK_VERSION} not discoverable (real-silicon only)",
        "Install it: from the workspace run `west sdk install` "
        "(not needed for native_sim).",
    )


def _is_windows() -> bool:
    return sys.platform.startswith("win")


def _check_git_autocrlf() -> CheckResult | None:
    if not _is_windows():
        return None
    try:
        proc = subprocess.run(
            ["git", "config", "--get", "core.autocrlf"],
            capture_output=True, text=True, timeout=10, check=False,
        )
        value = (proc.stdout or "").strip().lower()
    except (OSError, subprocess.SubprocessError):  # pragma: no cover - env-dependent
        return CheckResult(
            "git-autocrlf", WARN, "could not read git core.autocrlf",
            "Ensure `git config core.autocrlf` is not true (CRLF breaks west update).",
        )
    if value == "true":
        return CheckResult(
            "git-autocrlf", WARN, "git core.autocrlf=true (CRLF can break west update)",
            "git config --global core.autocrlf false (then re-clone if needed).",
        )
    return CheckResult("git-autocrlf", PASS, "git core.autocrlf is not true")


def _check_long_paths() -> CheckResult | None:
    if not _is_windows():
        return None
    try:  # pragma: no cover - Windows-only
        import winreg  # type: ignore

        key = winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Control\FileSystem"
        )
        value, _ = winreg.QueryValueEx(key, "LongPathsEnabled")
        winreg.CloseKey(key)
        if int(value) == 1:
            return CheckResult("long-paths", PASS, "Windows long-path support enabled")
    except (OSError, ValueError, TypeError):  # pragma: no cover - Windows-only
        # OSError: key/value absent. ValueError/TypeError: registry value is
        # not a clean integer -- a preflight check must degrade to WARN, never
        # crash the whole `alp doctor` run.
        pass
    return CheckResult(  # pragma: no cover - Windows-only
        "long-paths", WARN, "Windows long-path support not confirmed enabled",
        "Enable it: set HKLM\\SYSTEM\\...\\FileSystem\\LongPathsEnabled = 1 "
        "(long Zephyr build paths overflow MAX_PATH otherwise).",
    )


def _all_checks() -> list[CheckResult]:
    checks = [
        _check_python(),
        _check_west(),
        _check_python_deps(),
        _check_imgtool(),
        _check_cmake(),
        _check_ninja(),
        _check_dtc(),
        _check_gperf(),
        _check_host_compiler(),
        _check_zephyr_base(),
        _check_zephyr_version(),
        _check_west_workspace(),
        _check_workspace_venv(),
        _check_hal_alif(),
        _check_zephyr_sdk(),
        _check_jlink(),
        _check_tan(),
    ]
    # The pin check reads the repo's .python-version; skipped (None) on a
    # packaged install without the checkout.  Slot it beside the interpreter
    # floor check so the two Python lines read together.
    pin = _check_python_pin()
    if pin is not None:
        checks.insert(1, pin)
    for maybe in (_check_git_autocrlf(), _check_long_paths()):
        if maybe is not None:
            checks.append(maybe)
    return checks


# -------- rendering -----------------------------------------------------------

_STATUS_HUE = {PASS: Fore.GREEN, WARN: Fore.YELLOW, FAIL: Fore.RED}


def _render_line(result: CheckResult, color: bool) -> str:
    tag = f"[{result.status}]"
    if color:
        tag = f"{_STATUS_HUE[result.status]}{tag}{Style.RESET_ALL}"
    line = f"{tag} {result.name}: {result.message}"
    if result.hint and result.status != PASS:
        line += f"\n      -> hint: {result.hint}"
    return line


@click.command(name="doctor", help="Preflight: check the host build environment (HW-free).")
@click.option("--json", "as_json", is_flag=True, help="Emit a machine-readable JSON report.")
@click.option("--strict", is_flag=True, help="Treat WARN as failure (nonzero exit).")
@click.option("--no-color", is_flag=True, help="Disable ANSI colours.")
def doctor_cmd(as_json: bool, strict: bool, no_color: bool) -> None:
    """Inspect the host toolchain / workspace and report readiness.

    Read-only: no build, no board, no flash.  Exits 1 if any check FAILs (or,
    with --strict, if any check WARNs).
    """
    results = _all_checks()
    n_fail = sum(1 for r in results if r.status == FAIL)
    n_warn = sum(1 for r in results if r.status == WARN)
    failed = n_fail > 0 or (strict and n_warn > 0)

    if as_json:
        payload = {
            "checks": [
                {"name": r.name, "status": r.status, "message": r.message, "hint": r.hint}
                for r in results
            ],
            "summary": {
                "pass": sum(1 for r in results if r.status == PASS),
                "warn": n_warn,
                "fail": n_fail,
            },
            "strict": strict,
            "ok": not failed,
        }
        click.echo(_json.dumps(payload, indent=2))
    else:
        color = _use_color(False if no_color else None)
        for r in results:
            click.echo(_render_line(r, color))
        click.echo(
            f"\n{len(results)} checks: "
            f"{sum(1 for r in results if r.status == PASS)} pass, "
            f"{n_warn} warn, {n_fail} fail"
        )
        if failed:
            click.echo("doctor: environment is NOT ready (see hints above).", err=True)

    if failed:
        raise SystemExit(1)
