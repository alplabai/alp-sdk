"""`alp doctor` -- read-only environment preflight for the Alp SDK.

Runs a battery of host / filesystem / `--version` checks and prints one
`[PASS]`/`[WARN]`/`[FAIL]` line each with a remediation hint, mirroring the
truth encoded in `scripts/bootstrap.sh` (workspace venv at
`../zephyrproject/.venv`, the Zephyr pin read live from west.yml,
`.west`/`VERSION` probing).

It is strictly HW-free: no build, no board, no flash -- pure environment,
filesystem and `--version` inspection -- so it is safe to run anywhere,
anytime, before you ever touch silicon.

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
    """The Zephyr workspace bootstrap.sh creates beside the alp-sdk checkout."""
    return _repo_root().parent / "zephyrproject"


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
    if shutil.which("cmake") is None:
        return CheckResult(
            "cmake", FAIL, "cmake not found on PATH",
            "Install CMake 3.20+ (find_package(Zephyr) minimum).",
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
        "Upgrade CMake to 3.20+ (find_package(Zephyr) minimum).",
    )


def _check_ninja() -> CheckResult:
    # Zephyr's default CMake generator; every west build needs it.
    if shutil.which("ninja") is None:
        return CheckResult(
            "ninja", FAIL, "ninja not found on PATH",
            "Install it: apt install ninja-build / brew install ninja / "
            "winget install Ninja-build.Ninja.",
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
            "(bundled with the Zephyr SDK on Windows).",
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
            "(bundled with the Zephyr SDK on Windows).",
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


def _check_west_workspace() -> CheckResult:
    base = _zephyr_base()
    if base is None:
        return CheckResult(
            "west-workspace", WARN, "cannot check .west (ZEPHYR_BASE unset)",
            "Set ZEPHYR_BASE, then ensure <workspace>/.west exists (west init).",
        )
    if (base.parent / ".west").is_dir():
        return CheckResult("west-workspace", PASS, f"west workspace at {base.parent}")
    return CheckResult(
        "west-workspace", FAIL,
        f"no .west directory beside ZEPHYR_BASE ({base.parent})",
        "Initialise the workspace: run scripts/bootstrap.sh (or west init).",
    )


def _check_workspace_venv() -> CheckResult:
    # Prefer the venv beside the active ZEPHYR_BASE workspace; fall back to the
    # canonical ../zephyrproject/.venv that bootstrap.sh creates.
    candidates: list[Path] = []
    base = _zephyr_base()
    if base is not None:
        candidates.append(base.parent / ".venv")
    candidates.append(_workspace_dir() / ".venv")

    venv = next((c for c in candidates if c.is_dir()), None)
    if venv is None:
        return CheckResult(
            "workspace-venv", WARN, "workspace venv (../zephyrproject/.venv) not found",
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
    ]
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
