# SPDX-License-Identifier: Apache-2.0
#
# scripts/bootstrap.ps1
#
# Fresh-clone bootstrap for the Alp SDK on NATIVE Windows (PowerShell 7+).
# Mirrors scripts/bootstrap.sh: a Zephyr workspace beside the alp-sdk
# checkout, a workspace-local venv (west + Python deps), and an editable
# install of alp-sdk's internal/reference Python tooling (`alp_cli`) -- see
# docs/cross-platform-setup.md section 4 for the manual walkthrough
# this script automates.
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
# Expected directory layout after a successful run (PARENT is the west topdir;
# alp-sdk is the manifest repo -- `west init -l`, #769):
#
#     <parent>\                     (west topdir)
#     +-- alp-sdk\                  (this repo -- the workspace manifest)
#     +-- .west\
#     +-- .venv\                    (hermetic west + Zephyr/SDK Python deps)
#     +-- zephyr\                   (pin recorded in metadata/bootstrap.json,
#                                     from alp-sdk's west.yml -- #917)
#     +-- modules\
#
# Usage:
#
#     pwsh scripts\bootstrap.ps1                # full setup
#     pwsh scripts\bootstrap.ps1 -NoPip         # skip pip installs
#     pwsh scripts\bootstrap.ps1 -NoWest        # skip west init/update
#     pwsh scripts\bootstrap.ps1 -NoPatches     # skip `west patch apply` + its
#         # verification (issue #1392). The patches in zephyr/patches.yml are
#         # required to BUILD. Use this only when you intend to manage the
#         # patch state yourself.
#     pwsh scripts\bootstrap.ps1 -PrintEnv      # only print env-var lines
#     pwsh scripts\bootstrap.ps1 -AllowPartial
#         # report success even if zephyr-requirements / sdk-extras /
#         # editable-install failed to install (issue #1038); the failures
#         # are still printed and the workspace is left on disk either way --
#         # this only changes the closing verdict

[CmdletBinding()]
param(
    [switch]$NoPip,
    [switch]$NoWest,
    [switch]$NoPatches,
    [switch]$PrintEnv,
    [switch]$AllowPartial
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$ParentDir = (Resolve-Path (Join-Path $RepoRoot "..")).Path

# The west workspace topdir is the alp-sdk checkout's PARENT: we init the
# workspace from alp-sdk's OWN west.yml (`west init -l $RepoRoot`), so alp-sdk
# becomes the manifest repo and west discovers the alp-migrate / alp-lock /
# alp-quality / alp-emit extension commands from its `self.west-commands`
# (issue #769). `west init -l
# <repo>` always makes topdir = the repo's parent, and leaves alp-sdk itself in
# place; Zephyr + modules land as siblings of alp-sdk under the topdir.
$WorkspaceDir = $ParentDir
# The Zephyr version pin (and the other bootstrap facts below) is
# single-sourced from metadata/bootstrap.json (issue #917) -- kept in
# sync with the `revision:` pin in west.yml by
# scripts/check_bootstrap_manifest.py. Loaded further down, once the
# prereq check has confirmed the tools it needs exist. Used only to
# decide whether an existing $env:ZEPHYR_BASE tree is reusable --
# `west init -l` takes the actual revision from alp-sdk's west.yml.
$BootstrapJson = Join-Path $RepoRoot "metadata\bootstrap.json"

function Write-Info([string]$msg) { Write-Host "[bootstrap] $msg" -ForegroundColor Blue }
function Write-Ok([string]$msg)   { Write-Host "[bootstrap] $msg" -ForegroundColor Green }
function Write-Warn2([string]$msg){ Write-Host "[bootstrap] $msg" -ForegroundColor Yellow }
function Fail([string]$msg)       { Write-Host "[bootstrap] $msg" -ForegroundColor Red; exit 1 }

# Phase IDs (from metadata/bootstrap.json's verdict.blockingPhases, loaded
# further down as $VerdictBlockingPhases) whose pip install actually failed
# THIS run -- a subset of $VerdictBlockingPhases, filled in by
# Add-BlockingPhase as each pip step in "-------- pip dependencies --------"
# below completes. Read by the closing verdict logic at the end of the script.
$BlockingPhases = @()
function Add-BlockingPhase([string]$Phase) {
    # Appends to $BlockingPhases only when $VerdictBlockingPhases (metadata/
    # bootstrap.json, not hardcoded here) names it -- issue #1038: the
    # pip/wheel self-upgrade warns too (see the venv section below) but is
    # deliberately NOT in that list, so it must never reach this function.
    if ($script:VerdictBlockingPhases -contains $Phase) {
        $script:BlockingPhases += $Phase
    }
}

# -------- Prerequisite check --------------------------------------------------

# Print the exact winget one-liner for anything missing rather than
# attempting a system-wide install from a bootstrap script.
# Kept as a hardcoded list, not read from the manifest: reading the manifest
# below needs a working `python` to sanity-check ITS OWN version against the
# >= 3.10 floor further down (the non-PrintEnv path only -- see there), and
# this check is what confirms `python` exists in the first place for that --
# a bootstrap-of-the-bootstrap. Its agreement with metadata/bootstrap.json's
# `prerequisites.windows` is policed by scripts/check_bootstrap_manifest.py,
# not by this script.  Kept as ONE list (not two) so the gate's
# `$Prereqs = @(...)` regex still finds the full name set even though
# -PrintEnv below skips this check entirely.
$Prereqs = @(
    @{ Name = "git";    Hint = "winget install -e --id Git.Git" },
    @{ Name = "cmake";  Hint = "winget install -e --id Kitware.CMake" },
    @{ Name = "python"; Hint = "winget install -e --id Python.Python.3.12" },
    @{ Name = "ninja";  Hint = "winget install -e --id Ninja-build.Ninja" }
)
# -PrintEnv only reads metadata/bootstrap.json and prints. Unlike
# bootstrap.sh (which shells out to python3 to parse the manifest, so its
# --print-env genuinely needs python3 present), this script parses JSON with
# the native `ConvertFrom-Json` cmdlet -- no python involved -- and the
# python-version floor check below is skipped entirely for -PrintEnv too
# (see "Python version floor" below). So -PrintEnv needs NONE of
# git/cmake/python/ninja; skip the whole prerequisite check for it.
if ($PrintEnv) {
    $PrereqsToCheck = @()
} else {
    $PrereqsToCheck = $Prereqs
}
$Missing = @()
foreach ($p in $PrereqsToCheck) {
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

# -------- Load bootstrap facts (metadata/bootstrap.json, issue #917) ----------

# Single-source facts shared with scripts/bootstrap.sh and tan-cli (facts
# only -- control flow stays here; see the manifest's own "_comment").
# Deliberately placed after the prereq check above (same ordering rationale
# as bootstrap.sh): -PrintEnv (below) needs these facts, so it cannot
# short-circuit before this point either, even though it only prints. The
# Python-VERSION floor is NOT a prerequisite for this -- ConvertFrom-Json
# and the property reads below need no modern syntax -- so that check now
# sits below the -PrintEnv shortcut instead of up here (see there).
if (-not (Test-Path $BootstrapJson)) {
    Fail "missing $BootstrapJson"
}
$Manifest = Get-Content -Raw $BootstrapJson | ConvertFrom-Json
# Refuse a manifest shaped for a schema version this script doesn't
# understand -- otherwise a future v2 manifest gets parsed blind by this
# v1-shaped script on a machine where check_bootstrap_manifest.py never runs.
if ($Manifest.schemaVersion -ne 1) {
    Fail "metadata\bootstrap.json schemaVersion=$($Manifest.schemaVersion) -- this script only understands schemaVersion 1 (see scripts\check_bootstrap_manifest.py)."
}
function Resolve-BootstrapToken([string]$Value) {
    $Value.Replace('${SDK_ROOT}', $RepoRoot).Replace('${WORKSPACE_DIR}', $WorkspaceDir)
}
$ZephyrVersion            = Resolve-BootstrapToken $Manifest.zephyr.version
$ZephyrRequirementsPath   = Resolve-BootstrapToken $Manifest.zephyr.requirementsPath
$VenvDirName              = Resolve-BootstrapToken $Manifest.venv.dirName
$VenvWindowsBin           = Resolve-BootstrapToken $Manifest.venv.windowsBinDir
# The @(...) MUST wrap the WHOLE pipeline, not just its input: `@(X) |
# ForEach-Object {...}` only forces the INPUT into array context -- a
# single-element result still collapses PowerShell's pipeline output back
# down to a plain [string] on assignment, and `& $West @WestExportArgs`
# then splats that string CHARACTER BY CHARACTER ('z','e','p','h',...)
# instead of passing it as one argument.  `@(X | ForEach-Object {...})`
# forces the OUTPUT into a real array regardless of element count.
$WestPipSpec              = Resolve-BootstrapToken $Manifest.west.pipSpec
$WestInitArgs             = @($Manifest.west.initArgs   | ForEach-Object { Resolve-BootstrapToken $_ })
$WestUpdateArgs           = @($Manifest.west.updateArgs | ForEach-Object { Resolve-BootstrapToken $_ })
$WestExportArgs           = @($Manifest.west.exportArgs | ForEach-Object { Resolve-BootstrapToken $_ })
$WestExtGuard             = Resolve-BootstrapToken $Manifest.west.extensionGuardCommand
$PipBootstrapUpgrade      = @($Manifest.pip.bootstrapUpgrade | ForEach-Object { Resolve-BootstrapToken $_ })
$PipSdkExtras             = @($Manifest.pip.sdkExtras        | ForEach-Object { Resolve-BootstrapToken $_ })
$PipEditableInstall       = Resolve-BootstrapToken $Manifest.pip.editableInstall
# verdict: the single source for which pip phases make the closing verdict
# non-success, and the wording for it (issue #1038 / tan-cli#220) -- see
# metadata/schemas/bootstrap-v1.schema.json's verdict.* descriptions. No
# token substitution: these are phase IDs and prose, not paths.
$VerdictBlockingPhases           = @($Manifest.verdict.blockingPhases)
$VerdictPartialNoteTemplate      = $Manifest.verdict.partialNoteTemplate
$VerdictIncompleteMessageTemplate = $Manifest.verdict.incompleteMessageTemplate
$VerdictIncompleteRemedy         = $Manifest.verdict.incompleteRemedy
# env: ordered Name/RAW-Value pairs (JSON object property order is preserved
# by ConvertFrom-Json).  Token substitution is deliberately deferred to
# Write-EnvLines below, NOT done here at load time: the workspace-reuse
# block further down can reassign $WorkspaceDir (when an existing
# $env:ZEPHYR_BASE workspace is compatible), and -PrintEnv's early-exit call
# happens BEFORE that reassignment while the closing "Next steps" call
# happens AFTER it -- resolving here would bake in the pre-reuse
# $WorkspaceDir for the SECOND caller, printing a wrong $env:ZEPHYR_BASE.
# Mirrors bootstrap.sh's print_env_lines(), which re-reads $WORKSPACE_DIR
# fresh on every call for the same reason.
$EnvPairsRaw = $Manifest.env.PSObject.Properties | ForEach-Object {
    [PSCustomObject]@{ Name = $_.Name; RawValue = [string]$_.Value }
}
function Write-EnvLines([string]$Prefix = "") {
    foreach ($e in $EnvPairsRaw) {
        # Normalise to backslashes: substituting ${WORKSPACE_DIR} (bash-style
        # "\"-free path) into a manifest value like "${WORKSPACE_DIR}/zephyr"
        # (forward slash after the token) otherwise prints a mixed
        # "C:\Users\...\GitHub/zephyr" -- copy-paste-broken as a profile line.
        $val = (Resolve-BootstrapToken $e.RawValue) -replace '/', '\'
        Write-Host "$Prefix`$env:$($e.Name) = `"$val`""
    }
}
# manualInstallHints.windows.note: an ARRAY of lines (an aligned mapping
# reads better than one unwrapped paragraph) -- the Arm GNU Toolchain /
# Zephyr SDK manual-install fact, printed under "NOT auto-installed (manual,
# one-time):" further down. Deliberately NOT nativeLibHints (issue #917
# review item 7): nativeLibHints is an apt/brew/pkg-manager fact for the
# Yocto-side native libraries, which native Windows never installs at all
# (no heading for it exists in this script) -- printing nativeLibHints.
# windows.note here used to smuggle the unrelated toolchain sentence in
# under a "native libraries" framing it never had here, and separately made
# bootstrap.sh print that same sentence under ITS "native libraries" heading
# too, which was actively wrong (see manualInstallHints's schema comment).
$ManualInstallNote        = @($Manifest.manualInstallHints.windows.note)

# -------- Print-env shortcut --------------------------------------------------

if ($PrintEnv) {
    Write-Host "# Add to your PowerShell profile (or run before invoking the SDK):"
    Write-Host "# Activate the workspace venv (west + Zephyr/SDK Python deps live here):"
    Write-Host "#   & `"$WorkspaceDir\$VenvDirName\$VenvWindowsBin\Activate.ps1`""
    Write-EnvLines
    exit 0
}

# -------- Python version floor ------------------------------------------------

# Python >= 3.10 (dataclass slots, `X | None` unions in the tooling) --
# needed by the SDK tooling this venv/pip machinery is about to install and
# invoke, never by -PrintEnv, which exits above without ever reaching this
# check (ConvertFrom-Json needs no python at all -- see the Prerequisite
# check comment above). Deliberately placed AFTER the -PrintEnv shortcut so
# that early exit is real, not just theoretical. The "3.10" floor below is
# likewise hardcoded and policed against the manifest's
# `prerequisites.pythonMinVersion` by scripts/check_bootstrap_manifest.py --
# see the comment on $Prereqs above.
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
Write-Info "Workspace dir:   $WorkspaceDir  (west topdir; alp-sdk is the manifest)"
Write-Info "Python:          $PyVer"

# -------- workspace selection (reuse a compatible ZEPHYR_BASE) ----------------

# Mirrors bootstrap.sh. If $env:ZEPHYR_BASE points at a Zephyr tree whose
# MAJOR.MINOR matches our pin AND whose workspace manifest IS alp-sdk's, reuse
# it untouched. A workspace whose manifest is something else does NOT register
# the alp-* extension commands, so reusing it would leave `west alp-migrate`
# unknown (#769) -- ignore it and build our own instead.
$ReuseWs = $false
$PinMM = ($ZephyrVersion -replace '^v?(\d+\.\d+).*', '$1')
if ($env:ZEPHYR_BASE -and (Test-Path (Join-Path $env:ZEPHYR_BASE "VERSION"))) {
    $ExistTop = (Resolve-Path (Join-Path $env:ZEPHYR_BASE "..") -ErrorAction SilentlyContinue).Path
    $VerTxt   = Get-Content (Join-Path $env:ZEPHYR_BASE "VERSION") -Raw
    $Maj = if ($VerTxt -match 'VERSION_MAJOR\s*=\s*(\d+)') { $Matches[1] } else { "" }
    $Min = if ($VerTxt -match 'VERSION_MINOR\s*=\s*(\d+)') { $Matches[1] } else { "" }
    $ExistMM = "$Maj.$Min"
    # west/venv aren't set up yet, so read .west/config directly for the
    # manifest repo path.
    $ExistManifestDir = $null
    $CfgPath = if ($ExistTop) { Join-Path $ExistTop ".west\config" } else { $null }
    if ($CfgPath -and (Test-Path $CfgPath)) {
        $Rel = (Select-String -Path $CfgPath -Pattern '^\s*path\s*=\s*(.+)$' |
                Select-Object -First 1).Matches.Groups[1].Value
        if ($Rel) {
            $ExistManifestDir = (Resolve-Path (Join-Path $ExistTop $Rel.Trim()) -ErrorAction SilentlyContinue).Path
        }
    }
    if ($ExistTop -and (Test-Path (Join-Path $ExistTop ".west")) -and $ExistMM -eq $PinMM `
        -and $ExistManifestDir -eq $RepoRoot) {
        $ReuseWs = $true
        $WorkspaceDir = $ExistTop
        Write-Ok "Reusing compatible alp-sdk workspace from `$env:ZEPHYR_BASE: $WorkspaceDir (Zephyr $ExistMM.x)"
    } elseif ($ExistTop -and (Test-Path (Join-Path $ExistTop ".west")) -and $ExistMM -eq $PinMM) {
        Write-Warn2 "`$env:ZEPHYR_BASE workspace ($ExistTop) is a $PinMM.x tree but its manifest is not alp-sdk's west.yml"
        Write-Warn2 "-- not reusing it (would leave 'west alp-migrate' unknown, #769); building an alp-sdk workspace at $WorkspaceDir"
        $env:ZEPHYR_BASE = $null
    } else {
        Write-Warn2 "`$env:ZEPHYR_BASE ($env:ZEPHYR_BASE) is not a $PinMM.x west workspace -- ignoring it"
        $env:ZEPHYR_BASE = $null
    }
}

# -------- workspace venv (hermetic west + Python deps) ------------------------

# Everything -- west, the Zephyr requirements, the SDK extras, tan's Python
# backend -- installs into a workspace-local venv, never the system interpreter (same
# policy as bootstrap.sh; a global west couples the build to the host
# interpreter's state).  Idempotent: an existing venv is reused.
$VenvDir = Join-Path $WorkspaceDir $VenvDirName
$Vpy     = Join-Path $VenvDir "$VenvWindowsBin\python.exe"
$West    = Join-Path $VenvDir "$VenvWindowsBin\west.exe"

if (-not $NoWest -or -not $NoPip) {
    New-Item -ItemType Directory -Force $WorkspaceDir | Out-Null
    if (Test-Path $Vpy) {
        Write-Ok "Workspace venv already present at $VenvDir"
    } else {
        Write-Info "Creating workspace venv at $VenvDir"
        & python -m venv $VenvDir
        if ($LASTEXITCODE -ne 0) { Fail "python -m venv $VenvDir failed" }
    }
    & $Vpy -m pip install --upgrade -q @PipBootstrapUpgrade
    if ($LASTEXITCODE -ne 0) { Write-Warn2 "pip/wheel upgrade reported a problem" }
}

# -------- west init / update --------------------------------------------------

if (-not $NoWest) {
    if (-not (Test-Path $West)) {
        Write-Info "Installing west into the workspace venv ($WestPipSpec)"
        # Pinned to metadata/bootstrap.json's west.pipSpec -- the floor
        # Zephyr's own requirements-base.txt declares ("keep the version
        # identical to the minimum required in cmake/modules/west.cmake").
        & $Vpy -m pip install --upgrade -q $WestPipSpec
        if ($LASTEXITCODE -ne 0) { Fail "pip install west (venv) failed" }
    }

    if ($ReuseWs) {
        Write-Ok "Existing workspace reused -- skipping 'west init' / 'west update' (left untouched)"
    } elseif (-not (Test-Path (Join-Path $WorkspaceDir ".west"))) {
        Write-Info "Creating alp-sdk workspace at $WorkspaceDir (alp-sdk's west.yml is the manifest; takes a few minutes)"
        Push-Location $WorkspaceDir
        try {
            # -l makes alp-sdk ($RepoRoot) the manifest repo; topdir = its
            # parent = $WorkspaceDir. Zephyr (pinned in alp-sdk's west.yml) +
            # HALs + extras are fetched by `west update`. alp-sdk's
            # self.west-commands then exposes the alp-* extension commands in
            # this workspace (#769).
            & $West @WestInitArgs $RepoRoot
            if ($LASTEXITCODE -ne 0) { Fail "west init -l failed" }
            Write-Info "Running 'west update' (shallow + narrow; ~1.5 GB+ on a cold cache for zephyr/ + modules/, mostly vendor HALs -- this is a floor, not a ceiling; budget disk/bandwidth accordingly)"
            & $West @WestUpdateArgs
            if ($LASTEXITCODE -ne 0) { Fail "west update failed" }
            & $West @WestExportArgs
        } finally {
            Pop-Location
        }
    } else {
        Write-Ok "alp-sdk workspace already initialised at $WorkspaceDir"
        Push-Location $WorkspaceDir
        try {
            Write-Info "Running 'west update' (shallow + narrow)"
            & $West @WestUpdateArgs
            if ($LASTEXITCODE -ne 0) { Fail "west update failed" }
            & $West @WestExportArgs
        } finally {
            Pop-Location
        }
    }

    # Legibility guard (#769): fail at bootstrap time -- not at first
    # `tan build` -- if the workspace manifest doesn't register the alp-*
    # extension commands.
    if (-not $ReuseWs) {
        Push-Location $WorkspaceDir
        try {
            $WestHelp = & $West help 2>&1 | Out-String
        } finally {
            Pop-Location
        }
        if ($WestHelp -notmatch [regex]::Escape($WestExtGuard)) {
            Fail ("workspace at $WorkspaceDir does not register 'west alp-migrate' -- its manifest is not " +
                  "alp-sdk's west.yml (#769). Check 'west -C $WorkspaceDir config manifest.path'.")
        }
        Write-Ok "alp-* extension commands registered ('west alp-migrate' resolves in $WorkspaceDir)"
    }

} else {
    Write-Info "Skipping west setup (-NoWest)"
}

# -------- pip dependencies ----------------------------------------------------

if (-not $NoPip) {
    $ZephyrReqs = Join-Path $WorkspaceDir $ZephyrRequirementsPath
    if (Test-Path $ZephyrReqs) {
        Write-Info "Installing Zephyr Python requirements into the venv"
        & $Vpy -m pip install -q -r $ZephyrReqs
        if ($LASTEXITCODE -ne 0) {
            Write-Warn2 "Zephyr requirements install reported a problem -- check manually"
            Add-BlockingPhase "zephyr-requirements"
        }
    }
    # SDK-side extras: alp_project.py needs jsonschema; the MCUboot
    # dev-key script needs imgtool.
    Write-Info "Installing alp-sdk Python extras into the venv ($($PipSdkExtras -join ', '))"
    & $Vpy -m pip install -q @PipSdkExtras
    if ($LASTEXITCODE -ne 0) {
        Write-Warn2 "alp-sdk extras install reported a problem -- check manually"
        Add-BlockingPhase "sdk-extras"
    }
    # SDK-internal/reference Python tooling (including alp_cli) -- editable
    # install, so a `git pull` in the checkout updates it in place. Python
    # Tan is installed separately. During the v0.5 port, use tan-cli/dev in
    # its own Python 3.12+ venv; from v0.5 the installer supplies the frozen
    # runtime.
    Write-Info "Installing alp-sdk's internal Python tooling into the venv (pip install -e $PipEditableInstall)"
    & $Vpy -m pip install -q -e $PipEditableInstall
    if ($LASTEXITCODE -ne 0) {
        Write-Warn2 "alp_cli editable install reported a problem -- check manually"
        Add-BlockingPhase "editable-install"
    }
} else {
    Write-Info "Skipping pip installs (-NoPip)"
}

# -------- zephyr/patches.yml (issue #1392) ------------------------------------

# AFTER the pip section, not inside the west one: `west patch` imports
# `pykwalify.core` at module import time, and pykwalify arrives with the Zephyr
# requirements installed just above.
#
# VERIFY FIRST, then apply only what is missing. `west patch apply` is NOT
# idempotent: re-running it on an already-patched tree fails, because each
# patch is fed to `git apply` against content that already carries it. Measured
# on PR #1426's `getting-started` job, whose workspace cache key carries no
# commit component and so restored an already-patched tree.
#
# Mirrors scripts/bootstrap.sh's block of the same name. Without it a Windows
# user gets exactly the silently-unpatched workspace #1392 is about, while a
# Linux user does not.
if (-not $NoWest -and -not $NoPatches) {
    Push-Location $RepoRoot
    try {
        & $Vpy scripts/verify_west_patches.py --topdir $WorkspaceDir --west $West *> $null
        $VerifyRc = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    if ($VerifyRc -eq 0) {
        Write-Ok "zephyr/patches.yml already applied in $WorkspaceDir -- nothing to do"
    } else {
        # PER MODULE, not the whole set -- a workspace can be PARTIALLY
        # patched, and `west patch apply` is not idempotent. See
        # scripts/bootstrap.sh's block of the same name for the measurement.
        Push-Location $RepoRoot
        try {
            $Unapplied = & $Vpy scripts/verify_west_patches.py --topdir $WorkspaceDir --west $West --list-unapplied 2>$null
        } finally {
            Pop-Location
        }
        if (-not $Unapplied) {
            Fail "verify_west_patches.py reported patches missing (exit $VerifyRc) but named no module -- re-run it directly to see why"
        }
        foreach ($Mod in $Unapplied) {
            # `--dst-module` belongs to `west patch`, not to its `apply`
            # subcommand -- see scripts/bootstrap.sh's note of the same name.
            Write-Info "Applying zephyr/patches.yml for module '$Mod' ('west patch --dst-module ... apply')"
            Push-Location $WorkspaceDir
            try {
                & $West patch --dst-module $Mod apply
                if ($LASTEXITCODE -ne 0) { Fail "west patch --dst-module $Mod apply failed (exit $LASTEXITCODE) -- output above" }
            } finally {
                Pop-Location
            }
        }
        # Re-verify: `west patch apply`'s own exit status is not evidence it
        # did anything. Exit 3 is "everything present is patched, but a module
        # this workspace does not carry could not be checked" and warns; 1 and
        # 2 are the real thing.
        Push-Location $RepoRoot
        try {
            & $Vpy scripts/verify_west_patches.py --topdir $WorkspaceDir --west $West
            $VerifyRc = $LASTEXITCODE
        } finally {
            Pop-Location
        }
        if ($VerifyRc -eq 0) {
            Write-Ok "zephyr/patches.yml verified applied in $WorkspaceDir"
        } elseif ($VerifyRc -eq 3) {
            Write-Warn2 "some zephyr/patches.yml modules are not in this workspace -- see above"
        } else {
            Fail "zephyr/patches.yml is not applied in $WorkspaceDir (#1392) -- see the list above"
        }
    }
} elseif (-not $NoWest) {
    Write-Info "Skipping 'west patch apply' (-NoPatches) -- zephyr/patches.yml is NOT applied"
}

# -------- Manual-install hints ------------------------------------------------

Write-Host ""
Write-Info "NOT auto-installed (manual, one-time):"
# Rendered from metadata/bootstrap.json's `manualInstallHints.windows.note`
# (issue #917) -- not hardcoded here; edit the manifest to change this text.
# ARRAY of lines, one Write-Host per line, so the mapping stays aligned
# instead of collapsing into one unwrapped paragraph. Deliberately
# manualInstallHints, not nativeLibHints (review item 7): this text is
# about the manual Arm-toolchain/Zephyr-SDK install this heading names, not
# an Optional-native-library apt/brew fact -- see the comment on
# $ManualInstallNote above.
foreach ($line in $ManualInstallNote) {
    Write-Host "  $line"
}

# -------- Done ----------------------------------------------------------------

Write-Host ""
# The closing verdict (issue #1038 / tan-cli#220): every pip phase above
# stays non-fatal in itself -- the workspace is left on disk regardless of
# which packages failed -- but a run that hit one of
# metadata/bootstrap.json's verdict.blockingPhases has NOT produced an
# environment that can do what it was bootstrapped for, and must not report
# unqualified success. $BlockingPhases is populated by Add-BlockingPhase
# above; $Verdict* comes from the manifest, not hardcoded here, so this
# wording has exactly one declaration shared with scripts/bootstrap.sh (and,
# independently, tan-cli's WORKSPACE_BLOCKING).
#
# $ExitCode is set here but NOT acted on until the very end of this script
# (matching tan-cli's `verdict()`/`finish()` split): the Next steps block
# below -- including `tan doctor`, the tool that diagnoses exactly
# this kind of failure -- must still print on the incomplete path. Exiting
# here would take that away on the one run that needs it most.
$ExitCode = 0
if ($BlockingPhases.Count -eq 0) {
    Write-Ok "Bootstrap complete."
} elseif ($AllowPartial) {
    $PhasesJoined = $BlockingPhases -join ', '
    Write-Ok "Bootstrap complete."
    Write-Warn2 ("  " + ($VerdictPartialNoteTemplate -replace '\{\{PHASES\}\}', $PhasesJoined))
} else {
    $PhasesJoined = $BlockingPhases -join ', '
    Write-Warn2 ($VerdictIncompleteMessageTemplate -replace '\{\{PHASES\}\}', $PhasesJoined)
    Write-Warn2 "  $VerdictIncompleteRemedy"
    $ExitCode = 1
}
@"

Next steps:
  # Activate the workspace venv (west + Zephyr/SDK deps):
  & "$VenvDir\$VenvWindowsBin\Activate.ps1"

  # Make Zephyr reachable for builds:
"@ | Write-Host
Write-EnvLines "  "
@"

  # Sanity-check the host build environment (needs tan on PATH -- see
  # README.md for the current v0.5-transition install). Python Tan runs one
  # build/flash-oriented checklist; --build is a compatibility no-op.
  tan doctor

  # BUILDING YOUR OWN PROJECT -- the customer path. ``tan`` is the whole
  # command surface (ADR-0020), and ``tan build`` resolves the board from the
  # project's own board.yaml, so there is no -b to pass. ``tan examples`` lists
  # what you can start from. It always targets the real SKU your board.yaml
  # declares, so a real toolchain is required; ``tan doctor`` reports
  # whether you have one.
  tan init --name my-app --destination .. --sdk-root "$PWD"
  cd ../my-app; tan build

  # WORKING ON THE SDK ITSELF -- a contributor command, not part of building
  # your firmware. Spelling the Zephyr board target by hand is only needed on
  # this path:
  west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he ``
      examples\peripheral-io\uart-echo -- -DEXTRA_ZEPHYR_MODULES=$RepoRoot

References:
  - docs\cross-platform-setup.md  -- the full per-OS setup guide
  - docs\cli.md                   -- the tan CLI verb reference
"@ | Write-Host

# $ExitCode was decided by the closing verdict above (1 only on the
# INCOMPLETE path, i.e. -AllowPartial was not passed) -- deferred until here
# so the Next steps block always prints first, on every path (issue #1038 /
# tan-cli#220).
exit $ExitCode
