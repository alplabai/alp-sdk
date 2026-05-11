#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/bootstrap.sh
#
# Fresh-clone bootstrap for the ALP SDK.  Sets up a Zephyr workspace
# beside the alp-sdk checkout, installs Python deps, and prints the
# apt/brew commands for the optional native libraries the Yocto-side
# backends need (libmosquitto, libasound2, libssl).
#
# Idempotent -- re-running skips work that's already done.
#
# Expected directory layout after a successful run:
#
#     <parent>/
#     ├── alp-sdk/                  (this repo)
#     └── zephyrproject/
#         ├── .west/
#         ├── zephyr/               (v3.7.0 LTS pin from west.yml)
#         └── modules/
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
#     export ZEPHYR_BASE=$PWD/../zephyrproject/zephyr
#     bash scripts/test-all.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PARENT_DIR="$(cd "${REPO_ROOT}/.." && pwd)"

# Zephyr workspace lives one level up from the alp-sdk checkout so
# alp-sdk itself never gets relocated by `west init`.
WORKSPACE_DIR="${PARENT_DIR}/zephyrproject"
ZEPHYR_VERSION="v3.7.0"

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
            sed -n '3,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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

# -------- west setup ----------------------------------------------------------

if [ "${DO_WEST}" -eq 1 ]; then
    # Ensure pip-installable west is available; install user-locally if not.
    if ! command -v west >/dev/null 2>&1; then
        info "west not on PATH -- installing via pip (--user)"
        python3 -m pip install --user --upgrade west || die "pip install west failed"
        # Add the user-pip bin dir to PATH for this invocation.
        export PATH="$(python3 -m site --user-base)/bin:${PATH}"
    fi

    if [ ! -d "${WORKSPACE_DIR}/.west" ]; then
        info "Creating Zephyr workspace at ${WORKSPACE_DIR} (this takes a few minutes)"
        mkdir -p "${WORKSPACE_DIR}"
        ( cd "${WORKSPACE_DIR}" && \
          west init -m https://github.com/zephyrproject-rtos/zephyr \
                    --mr "${ZEPHYR_VERSION}" . \
        ) || die "west init failed"
    else
        ok "Zephyr workspace already initialised at ${WORKSPACE_DIR}"
    fi

    info "Running 'west update' (shallow + narrow; ~30 MB on a cold cache)"
    ( cd "${WORKSPACE_DIR}" && west update --narrow -o=--depth=1 ) \
        || die "west update failed"
    ( cd "${WORKSPACE_DIR}" && west zephyr-export ) || true
else
    info "Skipping west setup (--no-west)"
fi

# -------- pip dependencies ----------------------------------------------------

if [ "${DO_PIP}" -eq 1 ]; then
    if [ -f "${WORKSPACE_DIR}/zephyr/scripts/requirements.txt" ]; then
        info "Installing Zephyr Python requirements"
        python3 -m pip install --user --break-system-packages -q \
            -r "${WORKSPACE_DIR}/zephyr/scripts/requirements.txt" \
            || warn "Zephyr requirements install reported a problem -- check manually"
    fi
    # SDK-side extras: alp_project.py needs jsonschema; the MCUboot
    # dev-key script needs imgtool.
    info "Installing alp-sdk Python extras (jsonschema, imgtool)"
    python3 -m pip install --user --break-system-packages -q \
        jsonschema imgtool \
        || warn "alp-sdk extras install reported a problem -- check manually"
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
  # Make Zephyr reachable for builds:
  export ZEPHYR_BASE="${WORKSPACE_DIR}/zephyr"
  export ZEPHYR_TOOLCHAIN_VARIANT=zephyr

  # Run the local test suite:
  bash scripts/test-all.sh

  # Or jump straight into building an example:
  west build -b native_sim/native/64 examples/uart-echo \\
      -- -DEXTRA_ZEPHYR_MODULES=\$PWD

References:
  - docs/testing.md          -- full test-coverage map + how to run from scratch
  - docs/test-plan.md        -- per-feature verification ledger (⏳ / 🟡 / ✅)
EOF
