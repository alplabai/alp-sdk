# SPDX-License-Identifier: Apache-2.0
#
# scripts/bootstrap.ps1
#
# Fresh-clone bootstrap for the Alp SDK on NATIVE Windows (PowerShell 7+).
# Mirrors scripts/bootstrap.sh: a Zephyr workspace beside the alp-sdk
# checkout, a workspace-local venv (west + Python deps), and an editable
# install of the `alp` CLI -- see docs/cross-platform-setup.md section 4
# for the manual walkthrough this script automates.
#
# Honest scope -- what this script does NOT do:
#
#   * It does not install git / CMake / Python / Ninja themselves.  If a
#     prerequisite is missing it prints the matching `winget install`
#     one-liner and exits; installing system packages is your call.
#   * It does not install the Arm GNU Toolchain or the Zephyr SDK (both
#     are GUI/manual installs on Windows -- hints printed at the end).
#   * native_sim does not exist on native Windows; use WSL2 for the
#     simulator + Yocto halves (docs/cross-platform-setup.md section 5).
#
# Idempotent -- re-running skips work that's already done.
#
# Expected directory layout after a successful run:
#
#     <parent>\
#     +-- alp-sdk\                  (this repo)
#     +-- zephyrproject\
#         +-- .west\
#         +-- .venv\                (hermetic west + Zephyr/SDK Python deps)
#         +-- zephyr\               (v4.4.0 pin -- keep in sync with west.yml)
#         +-- modules\
#
# Usage:
#
#     pwsh scripts\bootstrap.ps1                # full setup
#     pwsh scripts\bootstrap.ps1 -NoPip         # skip pip installs
#     pwsh scripts\bootstrap.ps1 -NoWest        # skip west init/update
#     pwsh scripts\bootstrap.ps1 -PrintEnv      # only print env-var lines

[CmdletBinding()]
param(
    [switch]$NoPip,
    [switch]$NoWest,
    [switch]$PrintEnv
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$ParentDir = (Resolve-Path (Join-Path $RepoRoot "..")).Path

# Zephyr workspace lives one level up from the alp-sdk checkout so
# alp-sdk itself never gets relocated by `west init`.
$WorkspaceDir = Join-Path $ParentDir "zephyrproject"
# Keep in sync with the Zephyr `revision:` pin in the alp-sdk west.yml.
$ZephyrVersion = "v4.4.0"

function Write-Info([string]$msg) { Write-Host "[bootstrap] $msg" -ForegroundColor Blue }
function Write-Ok([string]$msg)   { Write-Host "[bootstrap] $msg" -ForegroundColor Green }
function Write-Warn2([string]$msg){ Write-Host "[bootstrap] $msg" -ForegroundColor Yellow }
function Fail([string]$msg)       { Write-Host "[bootstrap] $msg" -ForegroundColor Red; exit 1 }

# -------- Print-env shortcut --------------------------------------------------

if ($PrintEnv) {
    @"
# Add to your PowerShell profile (or run before invoking the SDK):
# Activate the workspace venv (west + Zephyr/SDK Python deps live here):
#   & "$WorkspaceDir\.venv\Scripts\Activate.ps1"
`$env:ZEPHYR_BASE = "$WorkspaceDir\zephyr"
`$env:ZEPHYR_TOOLCHAIN_VARIANT = "zephyr"
"@ | Write-Host
    exit 0
}

# -------- Prerequisite check --------------------------------------------------

# Print the exact winget one-liner for anything missing rather than
# attempting a system-wide install from a bootstrap script.
$Prereqs = @(
    @{ Name = "git";    Hint = "winget install -e --id Git.Git" },
    @{ Name = "cmake";  Hint = "winget install -e --id Kitware.CMake" },
    @{ Name = "python"; Hint = "winget install -e --id Python.Python.3.12" },
    @{ Name = "ninja";  Hint = "winget install -e --id Ninja-build.Ninja" }
)
$Missing = @()
foreach ($p in $Prereqs) {
    if (-not (Get-Command $p.Name -ErrorAction SilentlyContinue)) {
        $Missing += $p
    }
}
if ($Missing.Count -gt 0) {
    Write-Warn2 "Missing required tools:"
    foreach ($p in $Missing) {
        Write-Warn2 "  $($p.Name)  ->  $($p.Hint)"
    }
    Fail "Install the tools above (then reopen PowerShell) and re-run."
}

# Python >= 3.10 (dataclass slots, `X | None` unions in the tooling).
$PyVer = & python -c "import sys; print('%d.%d' % sys.version_info[:2])"
if (-not $PyVer) {
    # The Microsoft Store `python.exe` alias exists on PATH but prints
    # nothing (it opens the Store instead of running).
    Fail "python did not run (Windows Store alias?).  Install real Python: winget install -e --id Python.Python.3.12, reopen PowerShell, re-run."
}
if ([version]$PyVer -lt [version]"3.10") {
    Fail "Python $PyVer found; the SDK tooling needs >= 3.10 (winget install -e --id Python.Python.3.12)."
}

Write-Info "Repo root:       $RepoRoot"
Write-Info "Workspace dir:   $WorkspaceDir"
Write-Info "Python:          $PyVer"

# -------- workspace venv (hermetic west + Python deps) ------------------------

# Everything -- west, the Zephyr requirements, the SDK extras, the alp CLI --
# installs into a workspace-local venv, never the system interpreter (same
# policy as bootstrap.sh; a global west couples the build to the host
# interpreter's state).  Idempotent: an existing venv is reused.
$VenvDir = Join-Path $WorkspaceDir ".venv"
$Vpy     = Join-Path $VenvDir "Scripts\python.exe"
$West    = Join-Path $VenvDir "Scripts\west.exe"

if (-not $NoWest -or -not $NoPip) {
    New-Item -ItemType Directory -Force $WorkspaceDir | Out-Null
    if (Test-Path $Vpy) {
        Write-Ok "Workspace venv already present at $VenvDir"
    } else {
        Write-Info "Creating workspace venv at $VenvDir"
        & python -m venv $VenvDir
        if ($LASTEXITCODE -ne 0) { Fail "python -m venv $VenvDir failed" }
    }
    & $Vpy -m pip install --upgrade -q pip wheel
    if ($LASTEXITCODE -ne 0) { Write-Warn2 "pip/wheel upgrade reported a problem" }
}

# -------- west init / update --------------------------------------------------

if (-not $NoWest) {
    if (-not (Test-Path $West)) {
        Write-Info "Installing west into the workspace venv"
        & $Vpy -m pip install --upgrade -q west
        if ($LASTEXITCODE -ne 0) { Fail "pip install west (venv) failed" }
    }

    if (-not (Test-Path (Join-Path $WorkspaceDir ".west"))) {
        Write-Info "Creating Zephyr workspace at $WorkspaceDir (this takes a few minutes)"
        Push-Location $WorkspaceDir
        try {
            & $West init -m https://github.com/zephyrproject-rtos/zephyr --mr $ZephyrVersion .
            if ($LASTEXITCODE -ne 0) { Fail "west init failed" }
            Write-Info "Running 'west update' (shallow + narrow)"
            & $West update --narrow -o=--depth=1
            if ($LASTEXITCODE -ne 0) { Fail "west update failed" }
            & $West zephyr-export
        } finally {
            Pop-Location
        }
    } else {
        Write-Ok "Zephyr workspace already initialised at $WorkspaceDir"
        Push-Location $WorkspaceDir
        try {
            Write-Info "Running 'west update' (shallow + narrow)"
            & $West update --narrow -o=--depth=1
            if ($LASTEXITCODE -ne 0) { Fail "west update failed" }
            & $West zephyr-export
        } finally {
            Pop-Location
        }
    }
} else {
    Write-Info "Skipping west setup (-NoWest)"
}

# -------- pip dependencies ----------------------------------------------------

if (-not $NoPip) {
    $ZephyrReqs = Join-Path $WorkspaceDir "zephyr\scripts\requirements.txt"
    if (Test-Path $ZephyrReqs) {
        Write-Info "Installing Zephyr Python requirements into the venv"
        & $Vpy -m pip install -q -r $ZephyrReqs
        if ($LASTEXITCODE -ne 0) { Write-Warn2 "Zephyr requirements install reported a problem -- check manually" }
    }
    # SDK-side extras: alp_project.py needs jsonschema; the MCUboot
    # dev-key script needs imgtool.
    Write-Info "Installing alp-sdk Python extras into the venv (jsonschema, imgtool)"
    & $Vpy -m pip install -q jsonschema imgtool
    if ($LASTEXITCODE -ne 0) { Write-Warn2 "alp-sdk extras install reported a problem -- check manually" }
    # The `alp` CLI front door (alp init / build / run / flash / emit /
    # validate / model / doctor / monitor) -- editable install, so a
    # `git pull` in the checkout updates the CLI in place.
    Write-Info "Installing the alp CLI into the venv (pip install -e $RepoRoot)"
    & $Vpy -m pip install -q -e $RepoRoot
    if ($LASTEXITCODE -ne 0) { Write-Warn2 "alp CLI editable install reported a problem -- check manually" }
} else {
    Write-Info "Skipping pip installs (-NoPip)"
}

# -------- Manual-install hints ------------------------------------------------

Write-Host ""
Write-Info "NOT auto-installed (manual, one-time):"
@"

  # Arm GNU Toolchain (cross-compiles for real silicon) -- installer EXE:
  #   https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads
  #   (tick 'Add path to environment variable' during install)

  # Zephyr SDK (alternative cross-toolchain + host tools like dtc):
  #   run 'west sdk install' from $WorkspaceDir after this script.

  # native_sim / Yocto: not available on native Windows -- use WSL2
  #   (docs/cross-platform-setup.md section 5) and scripts/bootstrap.sh there.
"@ | Write-Host

# -------- Done ----------------------------------------------------------------

Write-Host ""
Write-Ok "Bootstrap complete."
@"

Next steps:
  # Activate the workspace venv (west + Zephyr/SDK deps + the alp CLI):
  & "$VenvDir\Scripts\Activate.ps1"

  # Make Zephyr reachable for builds:
  `$env:ZEPHYR_BASE = "$WorkspaceDir\zephyr"
  `$env:ZEPHYR_TOOLCHAIN_VARIANT = "zephyr"

  # Sanity-check the host environment with the alp CLI:
  alp doctor

  # Or jump straight into building an example for real silicon:
  west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he ``
      examples\peripheral-io\uart-echo -- -DEXTRA_ZEPHYR_MODULES=$RepoRoot

References:
  - docs\cross-platform-setup.md  -- the full per-OS setup guide
  - docs\cli.md                   -- the alp CLI verb reference
"@ | Write-Host
