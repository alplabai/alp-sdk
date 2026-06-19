#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/setup-clang-format.sh
#
# Pin the local `clang-format` to the version CI uses, so contributors
# don't waste commits chasing formatter-version mismatches.
#
# Background:
#   clang-format output is NOT stable across major versions -- the same
#   .clang-format reflows braces, trailing-comment columns, and
#   AlignConsecutive* differently from v14 to v18 to v22.  So CI PINS the
#   version, and local tooling MUST match it or the diff-only gate fails on
#   style-only churn even when no source actually changed.
#
#   We pin via the `clang-format` PIP WHEEL (clang-format==22.1.5), NOT apt:
#   apt's `clang-format` floats with the distro (Ubuntu 22.04 -> v14,
#   24.04 -> v18) and v22 is not packaged at all.  The wheel is the same
#   binary CI installs (.github/workflows/pr-static-analysis.yml +
#   pr-generated-files.yml) and is cross-platform (Linux / WSL / macOS /
#   Windows-bash).  It also ships `clang-format-diff.py`, which the gate uses.
#
# What this script does:
#   1. If a v22 clang-format is already reachable, report success.
#   2. Otherwise `pip install --user clang-format==<pin>` and add the wheel's
#      bin dir to PATH (printed for the caller to add to their profile).
#   3. Verify the final `clang-format --version` reports the pinned major.
#
# Modes:
#   (default)   install + verify.
#   --check     verify only (no install).  Exit 3 if local clang-format is
#               not the pinned version -- useful for pre-commit hooks.
#   -h, --help  print this header and exit.
#
# Exit codes:
#   0   local clang-format is now the pinned version (or already was).
#   1   install attempted but post-install verification failed.
#   2   no usable pip found / unsupported invocation.
#   3   --check mode and local clang-format is not the pinned version.
#
# Reference: docs/testing.md "Static analysis" + CONTRIBUTING.md "Formatting".

set -uo pipefail

REQUIRED_MAJOR=22
PIN="clang-format==22.1.5"

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
            sed -n '3,43p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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

# Extract the major version from a binary's `--version` output
# ("clang-format version 22.1.5").  Empty if missing/unparseable.
detect_version() {
    local bin="$1"
    command -v "${bin}" >/dev/null 2>&1 || { echo ""; return; }
    "${bin}" --version 2>/dev/null | grep -oE 'version [0-9]+' | awk '{print $2}' | head -1
}

# Prefer an explicitly-versioned binary, else plain `clang-format`.
CURRENT_BIN=""
CURRENT_VER=""
for cand in "clang-format-${REQUIRED_MAJOR}" clang-format; do
    v="$(detect_version "${cand}")"
    if [ -n "${v}" ]; then
        CURRENT_BIN="${cand}"; CURRENT_VER="${v}"; break
    fi
done

if [ "${CURRENT_VER}" = "${REQUIRED_MAJOR}" ]; then
    ok "clang-format is already v${REQUIRED_MAJOR}.x (via ${CURRENT_BIN}) -- matches CI"
    exit 0
fi

if [ "${CHECK_ONLY}" -eq 1 ]; then
    if [ -z "${CURRENT_BIN}" ]; then
        warn "clang-format is not installed; CI pins v${REQUIRED_MAJOR} (${PIN})"
    else
        warn "clang-format v${CURRENT_VER} found via ${CURRENT_BIN}; CI pins v${REQUIRED_MAJOR}"
        warn "Run scripts/setup-clang-format.sh (no --check) to install the pin"
    fi
    exit 3
fi

# -------- Install path (pip wheel -- cross-platform) --------------------------

PIP=""
for cand in "python3 -m pip" "python -m pip" pip3 pip; do
    # shellcheck disable=SC2086
    if ${cand} --version >/dev/null 2>&1; then PIP="${cand}"; break; fi
done
[ -n "${PIP}" ] || die 2 "no usable pip found -- install Python 3 + pip, then re-run (or: pip install --user '${PIN}')"

info "installing the pinned formatter:  ${PIP} install --user '${PIN}'"
# shellcheck disable=SC2086
${PIP} install --user "${PIN}" || die 1 "pip install failed"

# The wheel drops `clang-format` + `clang-format-diff.py` in the user-base bin.
USER_BIN="$(python3 -c 'import site,sys; print(site.USER_BASE + ("/Scripts" if sys.platform=="win32" else "/bin"))' 2>/dev/null || echo "$HOME/.local/bin")"
case ":$PATH:" in
    *":${USER_BIN}:"*) : ;;
    *) warn "add the wheel's bin to your PATH:  export PATH=\"${USER_BIN}:\$PATH\"" ;;
esac

NEW_VER="$(detect_version "${USER_BIN}/clang-format")"
[ -z "${NEW_VER}" ] && NEW_VER="$(detect_version clang-format)"
if [ "${NEW_VER}" = "${REQUIRED_MAJOR}" ]; then
    ok "clang-format v${REQUIRED_MAJOR}.x installed (${USER_BIN}/clang-format) -- matches CI"
    exit 0
fi
die 1 "post-install check failed: clang-format reports '${NEW_VER:-<none>}', expected v${REQUIRED_MAJOR}. Ensure ${USER_BIN} is on PATH ahead of any other clang-format."
