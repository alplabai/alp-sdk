#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/bootstrap.sh
#
# Cross-platform scope: this script targets Linux + macOS (POSIX
# shells).  Windows users should invoke it via WSL2 (Ubuntu-22.04
# is the tested distro) or run scripts/bootstrap.ps1 in native
# PowerShell (same flow: venv + west init/update + pip install -e .)
# -- see docs/cross-platform-setup.md section 4 for the manual
# equivalents and what the PS1 script cannot auto-install.
#
# Fresh-clone bootstrap for the Alp SDK.  Sets up a Zephyr workspace
# beside the alp-sdk checkout, installs Python deps, and prints the
# apt/brew commands for the optional native libraries the Yocto-side
# backends need (libmosquitto, libasound2, libssl).
#
# Idempotent -- re-running skips work that's already done.
#
# Expected directory layout after a successful run (the alp-sdk checkout's
# PARENT is the west topdir; alp-sdk is the manifest repo -- `west init -l`, #769):
#
#     <parent>/                     (west topdir)
#     ├── alp-sdk/                  (this repo -- the workspace manifest)
#     ├── .west/
#     ├── .venv/                    (hermetic west + Zephyr/SDK Python deps)
#     ├── zephyr/                   (v4.4.0 pin -- keep in sync with west.yml)
#     └── modules/                  (HALs + extras)
#
# Usage:
#
#     bash scripts/bootstrap.sh                # full setup
#     bash scripts/bootstrap.sh --no-pip       # skip pip installs
#     bash scripts/bootstrap.sh --no-west      # skip west init/update
#     bash scripts/bootstrap.sh --print-env    # only print env-var lines
#
# After it runs:
#
#     export ZEPHYR_BASE=$PWD/../zephyr
#     bash scripts/test-all.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PARENT_DIR="$(cd "${REPO_ROOT}/.." && pwd)"

# The west workspace topdir is the alp-sdk checkout's PARENT: we init the
# workspace from alp-sdk's OWN west.yml (`west init -l "${REPO_ROOT}"`), so
# alp-sdk becomes the manifest repo and west discovers the `alp-build`/`alp-flash`
# /... extension commands from its `self.west-commands` (issue #769). `west init
# -l <repo>` always makes topdir = dirname(<repo>) = PARENT_DIR, and leaves
# alp-sdk in place; Zephyr (pinned in that west.yml) + HALs + extras land as
# siblings of alp-sdk under the topdir -- the canonical alp-sdk workspace layout.
WORKSPACE_DIR="${PARENT_DIR}"
# Keep in sync with the Zephyr `revision:` pin in the alp-sdk west.yml
# (bumped v3.7.0 LTS -> v4.4.0 in v0.5).
ZEPHYR_VERSION="v4.4.0"

# -------- Flag parsing --------------------------------------------------------

DO_PIP=1
DO_WEST=1
PRINT_ENV_ONLY=0

while [ $# -gt 0 ]; do
    case "$1" in
        --no-pip)       DO_PIP=0 ;;
        --no-west)      DO_WEST=0 ;;
        --print-env)    PRINT_ENV_ONLY=1 ;;
        -h|--help)
            sed -n '3,31p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "bootstrap.sh: unknown flag '$1' (try --help)" >&2
            exit 2
            ;;
    esac
    shift
done

# -------- Output helpers ------------------------------------------------------

info() { printf "\033[1;34m[bootstrap]\033[0m %s\n" "$*"; }
ok()   { printf "\033[1;32m[bootstrap]\033[0m %s\n" "$*"; }
warn() { printf "\033[1;33m[bootstrap]\033[0m %s\n" "$*" >&2; }
die()  { printf "\033[1;31m[bootstrap]\033[0m %s\n" "$*" >&2; exit 1; }

# -------- Print-env shortcut --------------------------------------------------

if [ "${PRINT_ENV_ONLY}" -eq 1 ]; then
    cat <<EOF
# Add to your shell profile (or run before invoking the SDK):
# Activate the workspace venv (west + Zephyr/SDK Python deps live here):
#   source "${WORKSPACE_DIR}/.venv/bin/activate"
export ZEPHYR_BASE="${WORKSPACE_DIR}/zephyr"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
EOF
    exit 0
fi

# -------- Prerequisite check --------------------------------------------------

REQUIRED_BINS=(git cmake python3)
MISSING=()
for bin in "${REQUIRED_BINS[@]}"; do
    if ! command -v "${bin}" >/dev/null 2>&1; then
        MISSING+=("${bin}")
    fi
done
if [ "${#MISSING[@]}" -gt 0 ]; then
    die "Missing required tools: ${MISSING[*]}.  Install them and re-run."
fi

# Detect OS for the optional-native-libs hint at the end.
OS_LABEL="unknown"
case "$(uname -s)" in
    Linux)  OS_LABEL="linux" ;;
    Darwin) OS_LABEL="macos" ;;
    MINGW*|MSYS*|CYGWIN*) OS_LABEL="windows-bash" ;;
esac

info "Repo root:       ${REPO_ROOT}"
info "Workspace dir:   ${WORKSPACE_DIR}"
info "Detected OS:     ${OS_LABEL}"

# -------- workspace selection (reuse a compatible ZEPHYR_BASE) ----------------

# If ZEPHYR_BASE points at a Zephyr tree whose MAJOR.MINOR matches our pin and
# whose parent is a west workspace, reuse it (never modify the user's tree);
# otherwise ignore a stale ZEPHYR_BASE so it can't hijack `west init`, and build
# an isolated workspace.  Detection reads the ENVIRONMENT VARIABLE only -- never
# a shell rc file -- so it behaves identically under bash / zsh / fish / WSL.
PIN_MM="$(printf '%s' "${ZEPHYR_VERSION}" | sed -E 's/^v?([0-9]+\.[0-9]+).*/\1/')"
REUSE_WS=0
if [ -n "${ZEPHYR_BASE:-}" ] && [ -f "${ZEPHYR_BASE}/VERSION" ]; then
    EXIST_TOP="$(cd "${ZEPHYR_BASE}/.." 2>/dev/null && pwd || true)"
    EXIST_MM="$(awk -F= '
        /^VERSION_MAJOR/{gsub(/[^0-9]/,"",$2); j=$2}
        /^VERSION_MINOR/{gsub(/[^0-9]/,"",$2); n=$2}
        END{print j"."n}' "${ZEPHYR_BASE}/VERSION" 2>/dev/null)"
    # Only reuse a pre-existing workspace whose active manifest IS alp-sdk's
    # west.yml -- otherwise it does not register the `alp-*` extension commands
    # and `west alp-build` stays unknown (issue #769). west/venv aren't set up
    # yet here, so read .west/config directly for the manifest repo path.
    EXIST_MANIFEST_REL="$(sed -n 's/^[[:space:]]*path[[:space:]]*=[[:space:]]*//p' "${EXIST_TOP}/.west/config" 2>/dev/null | head -1)"
    EXIST_MANIFEST_DIR="$(cd "${EXIST_TOP}/${EXIST_MANIFEST_REL:-.}" 2>/dev/null && pwd || true)"
    if [ -n "${EXIST_TOP}" ] && [ -d "${EXIST_TOP}/.west" ] && [ "${EXIST_MM}" = "${PIN_MM}" ] \
       && [ "${EXIST_MANIFEST_DIR}" = "${REPO_ROOT}" ]; then
        REUSE_WS=1
        WORKSPACE_DIR="${EXIST_TOP}"
        ok "Reusing compatible alp-sdk workspace from \$ZEPHYR_BASE: ${WORKSPACE_DIR} (Zephyr ${EXIST_MM}.x)"
    elif [ -n "${EXIST_TOP}" ] && [ -d "${EXIST_TOP}/.west" ] && [ "${EXIST_MM}" = "${PIN_MM}" ]; then
        warn "\$ZEPHYR_BASE workspace (${EXIST_TOP}) is a ${PIN_MM}.x tree but its manifest is not alp-sdk's west.yml"
        warn "-- not reusing it (would leave 'west alp-build' unknown, #769); building an alp-sdk workspace at ${WORKSPACE_DIR}"
        unset ZEPHYR_BASE
    else
        warn "\$ZEPHYR_BASE (${ZEPHYR_BASE}) is not a ${PIN_MM}.x west workspace -- ignoring it and building an isolated one"
        unset ZEPHYR_BASE
    fi
fi

VENV_DIR="${WORKSPACE_DIR}/.venv"

# -------- workspace venv (hermetic west + Python deps) ------------------------

# Everything -- west, the Zephyr requirements, the SDK extras -- installs into a
# workspace-local venv, never the system interpreter / --user / --break-system-
# packages (issue #93: a half-removed system `packaging` once broke `west init`,
# and a global west couples the build to the host interpreter's state).  The
# alp CLI + VS Code extension auto-discover <workspace>/.venv, so this is
# backwards-compatible.  Idempotent: an existing venv is reused.
if [ "${DO_WEST}" -eq 1 ] || [ "${DO_PIP}" -eq 1 ]; then
    mkdir -p "${WORKSPACE_DIR}"
    if [ -x "${VENV_DIR}/bin/python" ] || [ -x "${VENV_DIR}/Scripts/python.exe" ]; then
        ok "Workspace venv already present at ${VENV_DIR}"
    else
        info "Creating workspace venv at ${VENV_DIR}"
        python3 -m venv "${VENV_DIR}" || die "python3 -m venv ${VENV_DIR} failed"
    fi
    # POSIX venvs put executables in bin/; a Windows (git-bash) venv uses Scripts/.
    if [ -d "${VENV_DIR}/bin" ]; then VBIN="${VENV_DIR}/bin"; else VBIN="${VENV_DIR}/Scripts"; fi
    VPY="${VBIN}/python"
    WEST="${VBIN}/west"
    "${VPY}" -m pip install --upgrade -q pip wheel || warn "pip/wheel upgrade reported a problem"
fi

# -------- west init / update --------------------------------------------------

if [ "${DO_WEST}" -eq 1 ]; then
    # west into the venv (NOT global / --user) so the system interpreter can't break it.
    if [ ! -x "${WEST}" ]; then
        info "Installing west into the workspace venv"
        "${VPY}" -m pip install --upgrade -q west || die "pip install west (venv) failed"
    fi

    if [ "${REUSE_WS}" -eq 1 ]; then
        ok "Existing workspace reused -- skipping 'west init' / 'west update' (left untouched)"
    elif [ ! -d "${WORKSPACE_DIR}/.west" ]; then
        info "Creating alp-sdk workspace at ${WORKSPACE_DIR} (alp-sdk's west.yml is the manifest; takes a few minutes)"
        # -l makes alp-sdk (REPO_ROOT) the manifest repo; topdir = its parent =
        # WORKSPACE_DIR. Zephyr (pinned in alp-sdk's west.yml) + HALs + extras are
        # fetched by `west update`. alp-sdk's self.west-commands then exposes the
        # alp-* extension commands in this workspace (#769).
        ( cd "${WORKSPACE_DIR}" && "${WEST}" init -l "${REPO_ROOT}" ) || die "west init -l failed"
        info "Running 'west update' (shallow + narrow; ~30 MB on a cold cache)"
        ( cd "${WORKSPACE_DIR}" && "${WEST}" update --narrow -o=--depth=1 ) || die "west update failed"
        ( cd "${WORKSPACE_DIR}" && "${WEST}" zephyr-export ) || true
    else
        ok "alp-sdk workspace already initialised at ${WORKSPACE_DIR}"
        info "Running 'west update' (shallow + narrow)"
        ( cd "${WORKSPACE_DIR}" && "${WEST}" update --narrow -o=--depth=1 ) || die "west update failed"
        ( cd "${WORKSPACE_DIR}" && "${WEST}" zephyr-export ) || true
    fi

    # Legibility guard (#769): fail at bootstrap time -- not at first `alp build`
    # -- if the workspace manifest doesn't register the alp-* extension commands.
    if [ "${REUSE_WS}" -eq 0 ]; then
        if ! ( cd "${WORKSPACE_DIR}" && "${WEST}" help 2>/dev/null | grep -q 'alp-build' ); then
            die "workspace at ${WORKSPACE_DIR} does not register 'west alp-build' -- its manifest is not alp-sdk's west.yml (#769). Check 'west -C ${WORKSPACE_DIR} config manifest.path'."
        fi
        ok "alp-* extension commands registered ('west alp-build' resolves in ${WORKSPACE_DIR})"
    fi

    # NOTE: this does NOT install the Zephyr SDK (the cross toolchains).
    # Real-silicon targets (e.g. the V2N M33-SM) require it -- run
    # `"${WEST}" sdk install` from "${WORKSPACE_DIR}" once after this step.
    # native_sim smoke builds use host gcc (ZEPHYR_TOOLCHAIN_VARIANT=host)
    # and don't need the SDK.
else
    info "Skipping west setup (--no-west)"
fi

# -------- pip dependencies ----------------------------------------------------

if [ "${DO_PIP}" -eq 1 ]; then
    # Into the SAME workspace venv -- no --user / --break-system-packages.
    if [ -f "${WORKSPACE_DIR}/zephyr/scripts/requirements.txt" ]; then
        info "Installing Zephyr Python requirements into the venv"
        "${VPY}" -m pip install -q \
            -r "${WORKSPACE_DIR}/zephyr/scripts/requirements.txt" \
            || warn "Zephyr requirements install reported a problem -- check manually"
    fi
    # SDK-side extras: alp_project.py needs jsonschema; the MCUboot
    # dev-key script needs imgtool.
    info "Installing alp-sdk Python extras into the venv (jsonschema, imgtool)"
    "${VPY}" -m pip install -q jsonschema imgtool \
        || warn "alp-sdk extras install reported a problem -- check manually"
    # The `alp` CLI front door (alp init / build / run / flash / emit /
    # validate / model / doctor / monitor) -- editable install, so a
    # `git pull` in the checkout updates the CLI in place.
    info "Installing the alp CLI into the venv (pip install -e ${REPO_ROOT})"
    "${VPY}" -m pip install -q -e "${REPO_ROOT}" \
        || warn "alp CLI editable install reported a problem -- check manually"
else
    info "Skipping pip installs (--no-pip)"
fi

# -------- Optional native libs hint -------------------------------------------

echo
info "Optional native libraries unlock the Yocto-side backends:"
case "${OS_LABEL}" in
    linux)
        cat <<EOF

  # libmosquitto-dev  -> alp_mqtt_* (cleartext + TLS)
  # libasound2-dev    -> alp_audio_*
  # libssl-dev        -> alp_hash_* / alp_aead_* / alp_random_bytes

  sudo apt-get install -y libmosquitto-dev libasound2-dev libssl-dev pkg-config
EOF
        ;;
    macos)
        cat <<EOF

  # Equivalents via Homebrew:
  brew install mosquitto pkg-config
  # Note: macOS uses CoreAudio rather than ALSA, so the Yocto audio
  # backend doesn't apply on macOS hosts.  OpenSSL ships with macOS.
EOF
        ;;
    windows-bash)
        cat <<EOF

  # Under Git Bash / MSYS2 the Yocto-side backends aren't intended to
  # run -- the canonical use is WSL2 + Ubuntu with the apt commands
  # above.  Skip this step on native Windows.
EOF
        ;;
    *)
        echo "  (OS not auto-detected; see docs/testing.md)"
        ;;
esac

# -------- Done ----------------------------------------------------------------

echo
ok "Bootstrap complete."
cat <<EOF

Next steps:
  # Activate the workspace venv (west + Zephyr/SDK deps live here):
  source "${VENV_DIR}/bin/activate"

  # Make Zephyr reachable for builds:
  export ZEPHYR_BASE="${WORKSPACE_DIR}/zephyr"
  export ZEPHYR_TOOLCHAIN_VARIANT=zephyr

  # Sanity-check the host environment with the alp CLI:
  alp doctor

  # Run the local test suite:
  bash scripts/test-all.sh

  # Or jump straight into building an example:
  west build -b native_sim/native/64 examples/peripheral-io/uart-echo \\
      -- -DEXTRA_ZEPHYR_MODULES=\$PWD

References:
  - docs/testing.md          -- full test-coverage map + how to run from scratch
  - docs/test-plan.md        -- per-feature verification ledger (⏳ / 🟡 / ✅)
EOF
