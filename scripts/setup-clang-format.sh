#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/setup-clang-format.sh
#
# Pin the local `clang-format` to the version CI uses, so contributors
# don't waste commits chasing formatter-version mismatches.
#
# Background:
#   CI installs `clang-format-14` (Ubuntu 22.04's apt default) and wires
#   it to /usr/bin/clang-format via update-alternatives -- see
#   .github/workflows/pr-static-analysis.yml.  The committed tree was
#   formatted against v14.  Floating with whatever the developer's
#   distro / Homebrew ships drifts: v18+ reflows braces, /**<-trailing-
#   comment column alignment, and AlignConsecutiveAssignments columns
#   differently from v14.  .clang-format pins Cpp11BracedListStyle to
#   cap one of those divergences, but the rest still leak.
#
# What this script does:
#   1. If a v14 clang-format is already reachable, report success.
#   2. Otherwise attempt a platform-appropriate install:
#        - Debian / Ubuntu: `apt-get install clang-format-14` and wire
#          /usr/bin/clang-format -> v14 via update-alternatives.
#        - macOS: `brew install llvm@14` (keg-only; prints a PATH line
#          for the caller to add to their shell profile).
#        - Other Linux / Windows-bash: print manual install instructions.
#   3. Verify the final `clang-format --version` reports 14.x.
#
# Modes:
#   (default)   install + verify.
#   --check     verify only (do not attempt install).  Exits non-zero
#               if local clang-format is not v14 -- useful for shell
#               pre-commit hooks or CI gates outside this repo.
#   -h, --help  print this header and exit.
#
# Exit codes:
#   0   local clang-format is now v14 (or already was).
#   1   install attempted but post-install verification failed.
#   2   platform not supported automatically; manual install required.
#   3   --check mode and local clang-format is not v14.
#
# Reference: docs/testing.md "Static analysis" section + CONTRIBUTING.md
# "Formatting" subsection.

set -uo pipefail

REQUIRED_MAJOR=14

# Output helpers -- match scripts/bootstrap.sh styling so a developer
# running both feels they're in the same toolchain.
info() { printf "\033[1;34m[clang-format-setup]\033[0m %s\n" "$*"; }
ok()   { printf "\033[1;32m[clang-format-setup]\033[0m %s\n" "$*"; }
warn() { printf "\033[1;33m[clang-format-setup]\033[0m %s\n" "$*" >&2; }
die()  { local rc="${1:-1}"; shift; printf "\033[1;31m[clang-format-setup]\033[0m %s\n" "$*" >&2; exit "${rc}"; }

# -------- Flag parsing --------------------------------------------------------

CHECK_ONLY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --check)        CHECK_ONLY=1 ;;
        -h|--help)
            sed -n '3,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "setup-clang-format.sh: unknown flag '$1' (try --help)" >&2
            exit 2
            ;;
    esac
    shift
done

# -------- Version probe -------------------------------------------------------

# Extract the major version from `clang-format --version` output of the
# form "Ubuntu clang-format version 14.0.0-1ubuntu1.1" / "clang-format
# version 17.0.6".  Empty string if the binary is missing or unparseable.
detect_version() {
    local bin="$1"
    command -v "${bin}" >/dev/null 2>&1 || { echo ""; return; }
    "${bin}" --version 2>/dev/null \
        | grep -oE 'version [0-9]+' \
        | awk '{print $2}' \
        | head -1
}

# Prefer an explicitly-versioned binary if one is installed; only fall
# back to plain `clang-format` if no -14 binary is on PATH.
CURRENT_BIN=""
CURRENT_VER=""
for cand in "clang-format-${REQUIRED_MAJOR}" clang-format; do
    v="$(detect_version "${cand}")"
    if [ -n "${v}" ]; then
        CURRENT_BIN="${cand}"
        CURRENT_VER="${v}"
        break
    fi
done

if [ "${CURRENT_VER}" = "${REQUIRED_MAJOR}" ]; then
    ok "clang-format is already v${REQUIRED_MAJOR}.x (via ${CURRENT_BIN}) -- matches CI"
    exit 0
fi

if [ "${CHECK_ONLY}" -eq 1 ]; then
    if [ -z "${CURRENT_BIN}" ]; then
        warn "clang-format is not installed; CI uses v${REQUIRED_MAJOR}"
    else
        warn "clang-format v${CURRENT_VER} found via ${CURRENT_BIN}; CI uses v${REQUIRED_MAJOR}"
        warn "Re-run scripts/setup-clang-format.sh (no --check) to install v${REQUIRED_MAJOR}"
    fi
    exit 3
fi

# -------- Install path --------------------------------------------------------

OS_LABEL="unknown"
case "$(uname -s)" in
    Linux)                OS_LABEL="linux" ;;
    Darwin)               OS_LABEL="macos" ;;
    MINGW*|MSYS*|CYGWIN*) OS_LABEL="windows-bash" ;;
esac

info "Detected OS:     ${OS_LABEL}"
if [ -n "${CURRENT_BIN}" ]; then
    info "Current binary:  ${CURRENT_BIN} (v${CURRENT_VER})"
else
    info "Current binary:  (none on PATH)"
fi
info "Target:          clang-format v${REQUIRED_MAJOR}.x (CI-pinned)"

case "${OS_LABEL}" in
    linux)
        if ! command -v apt-get >/dev/null 2>&1; then
            warn "Non-Debian Linux detected (apt-get not found)."
            cat >&2 <<EOF
  Manual install options:
    - Fedora / RHEL:  sudo dnf install clang-tools-extra
                      (verify the version reports 14 with
                       \`clang-format --version\`; older RHEL ships v14,
                       newer Fedora may ship v17+)
    - Arch / Manjaro: sudo pacman -S clang
                      (current; AUR has llvm14 if -Sy diverges)
    - Build from source:
        git clone --depth 1 --branch llvmorg-14.0.6 \\
            https://github.com/llvm/llvm-project
        cd llvm-project && cmake -S llvm -B build -G Ninja \\
            -DLLVM_ENABLE_PROJECTS=clang \\
            -DCMAKE_BUILD_TYPE=Release
        ninja -C build clang-format clang-format-diff
EOF
            exit 2
        fi
        info "Installing clang-format-${REQUIRED_MAJOR} via apt-get (sudo will prompt)"
        sudo apt-get update -qq \
            || die 1 "apt-get update failed"
        sudo apt-get install -y "clang-format-${REQUIRED_MAJOR}" \
            || die 1 "apt-get install clang-format-${REQUIRED_MAJOR} failed"
        # Make /usr/bin/clang-format point at v14 with high priority so
        # `clang-format` (unversioned) on PATH resolves the same as CI.
        sudo update-alternatives --install /usr/bin/clang-format \
            clang-format "/usr/bin/clang-format-${REQUIRED_MAJOR}" 100 \
            || warn "update-alternatives for clang-format failed; manual symlink may be needed"
        if command -v "clang-format-diff-${REQUIRED_MAJOR}" >/dev/null 2>&1; then
            sudo update-alternatives --install /usr/bin/clang-format-diff \
                clang-format-diff "/usr/bin/clang-format-diff-${REQUIRED_MAJOR}" 100 \
                || true
        fi
        ;;
    macos)
        if ! command -v brew >/dev/null 2>&1; then
            die 2 "Homebrew not found.  Install brew (https://brew.sh) and re-run."
        fi
        info "Installing llvm@${REQUIRED_MAJOR} via Homebrew"
        info "(this fetches the full LLVM ${REQUIRED_MAJOR} toolchain -- a few hundred MB)"
        brew install "llvm@${REQUIRED_MAJOR}" \
            || die 1 "brew install llvm@${REQUIRED_MAJOR} failed"
        # Homebrew's llvm@14 is keg-only; it does not symlink into
        # /usr/local/bin (Intel) or /opt/homebrew/bin (Apple Silicon).
        BREW_PREFIX="$(brew --prefix)"
        LLVM_BIN="${BREW_PREFIX}/opt/llvm@${REQUIRED_MAJOR}/bin"
        if [ ! -x "${LLVM_BIN}/clang-format" ]; then
            die 1 "expected ${LLVM_BIN}/clang-format after brew install -- not found"
        fi
        ok "Installed.  Add this line to your shell profile so v${REQUIRED_MAJOR} wins on PATH:"
        echo
        echo "    export PATH=\"${LLVM_BIN}:\$PATH\""
        echo
        info "Then re-open the shell (or 'source ~/.zshrc') and re-run --check."
        # Don't run the post-install version check below: we just told
        # the user to mutate PATH in their shell rc.  They'll re-verify.
        exit 0
        ;;
    windows-bash)
        cat >&2 <<EOF
Windows native install requires manual steps (no apt / brew available):

  1. Download the LLVM ${REQUIRED_MAJOR}.0.6 installer for Windows:
     https://github.com/llvm/llvm-project/releases/tag/llvmorg-${REQUIRED_MAJOR}.0.6
     (file: LLVM-${REQUIRED_MAJOR}.0.6-win64.exe)
  2. During install, pick "Add LLVM to system PATH for current user".
  3. Restart this shell and verify with:  clang-format --version

Alternative (recommended): develop inside WSL2 with Ubuntu 22.04 and
re-run this script there -- the apt path above works without manual
steps.
EOF
        exit 2
        ;;
    *)
        die 2 "Unsupported OS '$(uname -s)'.  Install clang-format ${REQUIRED_MAJOR}.x manually from https://releases.llvm.org/"
        ;;
esac

# -------- Post-install verify -------------------------------------------------

NEW_VER="$(detect_version clang-format)"
if [ "${NEW_VER}" != "${REQUIRED_MAJOR}" ]; then
    warn "post-install verification: \`clang-format\` reports v${NEW_VER:-<none>}, expected v${REQUIRED_MAJOR}"
    cat >&2 <<EOF
The v${REQUIRED_MAJOR} binary is on disk under its versioned name but the
unversioned \`clang-format\` resolves to something else.  Either:

  - sudo ln -sf "\$(command -v clang-format-${REQUIRED_MAJOR})" /usr/local/bin/clang-format
  - or prepend /usr/bin to PATH (apt's binaries land there).

Re-run \`scripts/setup-clang-format.sh --check\` to confirm.
EOF
    exit 1
fi
ok "clang-format is now v${REQUIRED_MAJOR}.x (matches CI pin)"
