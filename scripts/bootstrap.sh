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
#     ├── zephyr/                   (pin recorded in metadata/bootstrap.json,
#                                     kept in sync with west.yml -- #917)
#     └── modules/                  (HALs + extras)
#
# Usage: bash scripts/bootstrap.sh --help
#
#     (the flag list and the "after it runs" steps live in that --help
#     output, not here too -- one copy, not two hand-synced ones; see the
#     comment on the `-h|--help` case below.)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PARENT_DIR="$(cd "${REPO_ROOT}/.." && pwd)"

# The west workspace topdir is the alp-sdk checkout's PARENT: we init the
# workspace from alp-sdk's OWN west.yml (`west init -l "${REPO_ROOT}"`), so
# alp-sdk becomes the manifest repo and west discovers the `alp-migrate`/`alp-lock`
# /`alp-quality`/`alp-emit` extension commands from its `self.west-commands`
# (issue #769). `west init -l <repo>` always makes topdir = dirname(<repo>) =
# PARENT_DIR, and leaves
# alp-sdk in place; Zephyr (pinned in that west.yml) + HALs + extras land as
# siblings of alp-sdk under the topdir -- the canonical alp-sdk workspace layout.
WORKSPACE_DIR="${PARENT_DIR}"
# The Zephyr version pin (and the other bootstrap facts below) is
# single-sourced from metadata/bootstrap.json (issue #917) -- kept in
# sync with the `revision:` pin in west.yml by
# scripts/check_bootstrap_manifest.py. Loaded further down, once the
# prereq check has confirmed python3 exists.
BOOTSTRAP_JSON="${REPO_ROOT}/metadata/bootstrap.json"

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
            # The SINGLE authoritative copy of the usage text (issue #917
            # review item 11): the header comment above deliberately does
            # NOT restate the flag list or the "after it runs" steps, so
            # there is nothing left to hand-sync between two copies -- a
            # hardcoded `sed -n 'N,Mp'` line-range slice of a header
            # comment was tried before this and silently truncated the
            # printed usage the next time someone edited the header (the
            # range ended mid-comment, cutting the invocation lines and the
            # "After it runs" block); a duplicated-by-eye copy of the header
            # text replaced that but was itself the same drift risk one
            # level removed. Quoted heredoc tag (<<'EOF') so none of this
            # block's own text is treated as shell interpolation.
            cat <<'EOF'
Usage:

    bash scripts/bootstrap.sh                # full setup
    bash scripts/bootstrap.sh --no-pip       # skip pip installs
    bash scripts/bootstrap.sh --no-west      # skip west init/update
    bash scripts/bootstrap.sh --print-env    # only print env-var lines

After it runs:

    export ZEPHYR_BASE=$PWD/../zephyr
    bash scripts/test-all.sh
EOF
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

# -------- Prerequisite check --------------------------------------------------

# Kept as ONE canonical hardcoded list (not read from the manifest: reading
# the manifest itself needs python3, one of these prerequisites --
# restructuring this into a manifest read would be a
# bootstrap-of-the-bootstrap). Its agreement with metadata/bootstrap.json's
# `prerequisites.posix` is policed by scripts/check_bootstrap_manifest.py,
# whose regex expects the full array assignment right below, not by this
# script.
REQUIRED_BINS=(git cmake python3)
# --print-env only reads metadata/bootstrap.json and prints -- it never
# touches git/cmake, so it only needs python3 present.
if [ "${PRINT_ENV_ONLY}" -eq 1 ]; then
    REQUIRED_BINS=(python3)
fi
MISSING=()
for bin in "${REQUIRED_BINS[@]}"; do
    if ! command -v "${bin}" >/dev/null 2>&1; then
        MISSING+=("${bin}")
    fi
done
if [ "${#MISSING[@]}" -gt 0 ]; then
    die "Missing required tools: ${MISSING[*]}.  Install them and re-run."
fi

# -------- Load bootstrap facts (metadata/bootstrap.json, issue #917) ----------

# Single-source facts shared with scripts/bootstrap.ps1 and tan-cli (facts
# only -- control flow stays here; see the manifest's own "_comment").  Reads
# with python3 + stdlib json ONLY (jsonschema/PyYAML aren't installed yet --
# this script is what installs them, and the JSON-loading snippet below
# deliberately uses only baseline python syntax, nothing that needs the
# >= 3.10 floor checked further down).  Deliberately placed AFTER the prereq
# check above: a machine missing python3 must see the curated `die` message,
# not a raw "python3: command not found" -- this is also why --print-env
# (below) cannot short-circuit BEFORE this point even though it only prints
# facts: it needs those facts, and reading them needs python3 to already be
# confirmed present. It does NOT need the >= 3.10 floor, which is why that
# check now sits below the --print-env shortcut instead of up here.
[ -f "${BOOTSTRAP_JSON}" ] || die "missing ${BOOTSTRAP_JSON}"
# Capture the python output into a variable FIRST, then eval it as a
# separate statement -- `eval "$(cmd)"` on its own swallows a python3
# failure silently: under `set -uo pipefail` (no `-e`), eval's own exit
# status is that of the LAST line in the substituted text (or 0 if the
# substitution is empty), never python3's.  A crash here used to leave a
# partially-loaded fact set with no error at all.
#
# Quoted heredoc tag (<<'PY', not <<PY): lets the python source use real
# apostrophes in its own prose (below) without escaping to survive a
# single-quoted `python3 -c '...'` string -- this used to read "pythons
# tok() or bashs own" because the apostrophes had to be dropped.
_facts="$(python3 - "${BOOTSTRAP_JSON}" "${REPO_ROOT}" "${WORKSPACE_DIR}" <<'PY'
import json, shlex, sys
path, sdk_root, ws_dir = sys.argv[1], sys.argv[2], sys.argv[3]
d = json.load(open(path, encoding="utf-8"))
def tok(s):
    return s.replace("${SDK_ROOT}", sdk_root).replace("${WORKSPACE_DIR}", ws_dir)
def emit(name, value):
    # Lists become real bash ARRAY literals with each element quoted
    # separately.  Joining them into one string and re-splitting with
    # `read -ra` would split on spaces INSIDE an element too -- a
    # Windows path like C:/Users/John Smith/alp-sdk would arrive at
    # `west init -l` as two arguments.
    if isinstance(value, list):
        print(name + "=(" + " ".join(shlex.quote(tok(v)) for v in value) + ")")
    else:
        print(name + "=" + shlex.quote(tok(str(value))))
emit("SCHEMA_VERSION", d["schemaVersion"])
emit("ZEPHYR_VERSION", d["zephyr"]["version"])
emit("ZEPHYR_REQUIREMENTS_PATH", d["zephyr"]["requirementsPath"])
emit("VENV_DIR_NAME", d["venv"]["dirName"])
emit("VENV_POSIX_BIN", d["venv"]["posixBinDir"])
emit("VENV_WINDOWS_BIN", d["venv"]["windowsBinDir"])
emit("WEST_PIP_SPEC", d["west"]["pipSpec"])
emit("WEST_INIT_ARGS", d["west"]["initArgs"])
emit("WEST_UPDATE_ARGS", d["west"]["updateArgs"])
emit("WEST_EXPORT_ARGS", d["west"]["exportArgs"])
emit("WEST_EXT_GUARD", d["west"]["extensionGuardCommand"])
emit("PIP_BOOTSTRAP_UPGRADE", d["pip"]["bootstrapUpgrade"])
emit("PIP_SDK_EXTRAS", d["pip"]["sdkExtras"])
emit("PIP_EDITABLE_INSTALL", d["pip"]["editableInstall"])
# env: keys and RAW (untokenized) values as two parallel arrays (bash has
# no portable ordered-map array type across the bash 3.2 macOS ships and
# bash 4+). Token substitution happens in bash, not here: git-bash silently
# rewrites a POSIX-style path argument (e.g. "/c/Users/x") into
# "C:/Users/x" when handed to a native (non-MSYS) python.exe, which would
# make this same directory print two different ways depending on whether
# it went through python's tok() or bash's own $WORKSPACE_DIR -- see
# print_env_lines() below.
emit("ENV_KEYS", list(d["env"].keys()))
print("ENV_VALS_RAW=(" + " ".join(shlex.quote(str(v)) for v in d["env"].values()) + ")")
# nativeLibHints: a note (an ARRAY of lines -- an aligned mapping reads
# better than one unwrapped paragraph) + one (possibly empty) command per OS.
for os_key in ("linux", "macos", "windows"):
    hint = d["nativeLibHints"][os_key]
    emit("HINT_" + os_key.upper() + "_NOTE", hint["note"])
    emit("HINT_" + os_key.upper() + "_CMD", hint["command"] or "")
PY
)" || die "failed to read ${BOOTSTRAP_JSON} (see scripts/check_bootstrap_manifest.py)"
eval "${_facts}"

# Refuse a manifest shaped for a schema version this script doesn't
# understand -- otherwise a future v2 manifest gets parsed blind by this
# v1-shaped script on a machine where check_bootstrap_manifest.py never
# runs.
[ "${SCHEMA_VERSION}" = "1" ] || die "metadata/bootstrap.json schemaVersion=${SCHEMA_VERSION} -- this script only understands schemaVersion 1 (see scripts/check_bootstrap_manifest.py)."

# -------- env-line helper (shared by --print-env and the closing summary) -----

# Renders metadata/bootstrap.json's `env` map as `export KEY=VALUE` lines --
# quoted only when the value looks like a path (contains "/"), matching the
# hand-written formatting this replaces byte-for-byte.  Token-substitutes
# ${SDK_ROOT}/${WORKSPACE_DIR} with bash's OWN variables (not python's --
# see the ENV_VALS_RAW comment above) so the path style is whatever bash
# itself would print, and reads $WORKSPACE_DIR fresh each call since the
# workspace-reuse logic further down can reassign it after --print-env
# has already returned.  Optional $1 is a per-line prefix (e.g. "  " for
# indenting under "Next steps:").
print_env_lines() {
    local prefix="${1:-}"
    local i key val
    for i in "${!ENV_KEYS[@]}"; do
        key="${ENV_KEYS[$i]}"
        val="${ENV_VALS_RAW[$i]}"
        val="${val//\$\{SDK_ROOT\}/${REPO_ROOT}}"
        val="${val//\$\{WORKSPACE_DIR\}/${WORKSPACE_DIR}}"
        case "${val}" in
            */*) printf '%sexport %s="%s"\n' "${prefix}" "${key}" "${val}" ;;
            *)   printf '%sexport %s=%s\n'   "${prefix}" "${key}" "${val}" ;;
        esac
    done
}

# -------- Print-env shortcut --------------------------------------------------

if [ "${PRINT_ENV_ONLY}" -eq 1 ]; then
    printf '# Add to your shell profile (or run before invoking the SDK):\n'
    printf '# Activate the workspace venv (west + Zephyr/SDK Python deps live here):\n'
    printf '#   source "%s/%s/%s/activate"\n' "${WORKSPACE_DIR}" "${VENV_DIR_NAME}" "${VENV_POSIX_BIN}"
    print_env_lines
    exit 0
fi

# -------- Python version floor -------------------------------------------------

# python3 >= <floor> (dataclass slots, `X | None` unions the SDK's own
# Python tooling -- alp_project.py, alp_orchestrate.py -- uses).  Deliberately
# placed AFTER the --print-env shortcut above (unlike the git/cmake/python3
# PRESENCE check up in "Prerequisite check", this is a version-of-python3
# check): --print-env only loads and prints the manifest facts, which needs
# python3 present but never touches the SDK tooling this floor protects, so
# it must not pay this cost or fail on an old-but-present python3.  Hardcoded
# (same bootstrap-of-the-bootstrap rationale as REQUIRED_BINS above) and
# policed against metadata/bootstrap.json's `prerequisites.pythonMinVersion`
# by scripts/check_bootstrap_manifest.py.
PYTHON_MIN_VERSION="3.10"
PY_VER="$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
if ! python3 -c "
import sys
floor = tuple(int(x) for x in '${PYTHON_MIN_VERSION}'.split('.'))
sys.exit(0 if sys.version_info[:2] >= floor else 1)
"; then
    die "python3 ${PY_VER} found; the SDK tooling needs >= ${PYTHON_MIN_VERSION}."
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
    # and `west alp-migrate` stays unknown (issue #769). west/venv aren't set up
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
        warn "-- not reusing it (would leave 'west alp-migrate' unknown, #769); building an alp-sdk workspace at ${WORKSPACE_DIR}"
        unset ZEPHYR_BASE
    else
        warn "\$ZEPHYR_BASE (${ZEPHYR_BASE}) is not a ${PIN_MM}.x west workspace -- ignoring it and building an isolated one"
        unset ZEPHYR_BASE
    fi
fi

VENV_DIR="${WORKSPACE_DIR}/${VENV_DIR_NAME}"

# -------- workspace venv (hermetic west + Python deps) ------------------------

# Everything -- west, the Zephyr requirements, the SDK extras -- installs into a
# workspace-local venv, never the system interpreter / --user / --break-system-
# packages (issue #93: a half-removed system `packaging` once broke `west init`,
# and a global west couples the build to the host interpreter's state).
# tan's Python backend (alp_cli) + the VS Code extension auto-discover
# <workspace>/.venv, so this is backwards-compatible.  Idempotent: an
# existing venv is reused.
if [ "${DO_WEST}" -eq 1 ] || [ "${DO_PIP}" -eq 1 ]; then
    mkdir -p "${WORKSPACE_DIR}"
    if [ -x "${VENV_DIR}/${VENV_POSIX_BIN}/python" ] || [ -x "${VENV_DIR}/${VENV_WINDOWS_BIN}/python.exe" ]; then
        ok "Workspace venv already present at ${VENV_DIR}"
    else
        info "Creating workspace venv at ${VENV_DIR}"
        python3 -m venv "${VENV_DIR}" || die "python3 -m venv ${VENV_DIR} failed"
    fi
    # POSIX venvs put executables in bin/; a Windows (git-bash) venv uses Scripts/.
    if [ -d "${VENV_DIR}/${VENV_POSIX_BIN}" ]; then VBIN="${VENV_DIR}/${VENV_POSIX_BIN}"; else VBIN="${VENV_DIR}/${VENV_WINDOWS_BIN}"; fi
    VPY="${VBIN}/python"
    WEST="${VBIN}/west"
    "${VPY}" -m pip install --upgrade -q "${PIP_BOOTSTRAP_UPGRADE[@]}" || warn "pip/wheel upgrade reported a problem"
fi

# -------- west init / update --------------------------------------------------

if [ "${DO_WEST}" -eq 1 ]; then
    # west into the venv (NOT global / --user) so the system interpreter can't break it.
    if [ ! -x "${WEST}" ]; then
        info "Installing west into the workspace venv (${WEST_PIP_SPEC})"
        # Pinned to metadata/bootstrap.json's west.pipSpec -- the floor
        # Zephyr's own requirements-base.txt declares ("keep the version
        # identical to the minimum required in cmake/modules/west.cmake").
        "${VPY}" -m pip install --upgrade -q "${WEST_PIP_SPEC}" || die "pip install west (venv) failed"
    fi

    if [ "${REUSE_WS}" -eq 1 ]; then
        ok "Existing workspace reused -- skipping 'west init' / 'west update' (left untouched)"
    elif [ ! -d "${WORKSPACE_DIR}/.west" ]; then
        info "Creating alp-sdk workspace at ${WORKSPACE_DIR} (alp-sdk's west.yml is the manifest; takes a few minutes)"
        # -l makes alp-sdk (REPO_ROOT) the manifest repo; topdir = its parent =
        # WORKSPACE_DIR. Zephyr (pinned in alp-sdk's west.yml) + HALs + extras are
        # fetched by `west update`. alp-sdk's self.west-commands then exposes the
        # alp-* extension commands in this workspace (#769).
        ( cd "${WORKSPACE_DIR}" && "${WEST}" "${WEST_INIT_ARGS[@]}" "${REPO_ROOT}" ) || die "west init -l failed"
        info "Running 'west update' (shallow + narrow; ~30 MB on a cold cache)"
        ( cd "${WORKSPACE_DIR}" && "${WEST}" "${WEST_UPDATE_ARGS[@]}" ) || die "west update failed"
        ( cd "${WORKSPACE_DIR}" && "${WEST}" "${WEST_EXPORT_ARGS[@]}" ) || true
    else
        ok "alp-sdk workspace already initialised at ${WORKSPACE_DIR}"
        info "Running 'west update' (shallow + narrow)"
        ( cd "${WORKSPACE_DIR}" && "${WEST}" "${WEST_UPDATE_ARGS[@]}" ) || die "west update failed"
        ( cd "${WORKSPACE_DIR}" && "${WEST}" "${WEST_EXPORT_ARGS[@]}" ) || true
    fi

    # Legibility guard (#769): fail at bootstrap time -- not at first `tan build`
    # -- if the workspace manifest doesn't register the alp-* extension commands.
    if [ "${REUSE_WS}" -eq 0 ]; then
        if ! ( cd "${WORKSPACE_DIR}" && "${WEST}" help 2>/dev/null | grep -q "${WEST_EXT_GUARD}" ); then
            die "workspace at ${WORKSPACE_DIR} does not register 'west alp-migrate' -- its manifest is not alp-sdk's west.yml (#769). Check 'west -C ${WORKSPACE_DIR} config manifest.path'."
        fi
        ok "alp-* extension commands registered ('west alp-migrate' resolves in ${WORKSPACE_DIR})"
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
    if [ -f "${WORKSPACE_DIR}/${ZEPHYR_REQUIREMENTS_PATH}" ]; then
        info "Installing Zephyr Python requirements into the venv"
        "${VPY}" -m pip install -q \
            -r "${WORKSPACE_DIR}/${ZEPHYR_REQUIREMENTS_PATH}" \
            || warn "Zephyr requirements install reported a problem -- check manually"
    fi
    # SDK-side extras: alp_project.py needs jsonschema; the MCUboot
    # dev-key script needs imgtool.
    info "Installing alp-sdk Python extras into the venv (${PIP_SDK_EXTRAS[*]})"
    "${VPY}" -m pip install -q "${PIP_SDK_EXTRAS[@]}" \
        || warn "alp-sdk extras install reported a problem -- check manually"
    # tan's Python backend (alp_cli: init / run / emit / validate / model /
    # doctor / monitor, invoked as `python -m alp_cli <sub>` by `tan`) --
    # editable install, so a `git pull` in the checkout updates the backend
    # in place. `tan` itself is a separate Rust binary, installed via
    # `cargo install --git https://github.com/alplabai/tan-cli --bin tan`.
    info "Installing the tan CLI's Python backend into the venv (pip install -e ${PIP_EDITABLE_INSTALL})"
    "${VPY}" -m pip install -q -e "${PIP_EDITABLE_INSTALL}" \
        || warn "alp_cli editable install reported a problem -- check manually"
else
    info "Skipping pip installs (--no-pip)"
fi

# -------- Optional native libs hint -------------------------------------------

# Rendered from metadata/bootstrap.json's `nativeLibHints` (issue #917) --
# not hardcoded here; edit the manifest to change this text. This script is
# the sole consumer of nativeLibHints today: bootstrap.ps1 has no "native
# libraries" heading of its own (native Windows never installs the
# Yocto-side native libs directly) -- see `manualInstallHints` for the
# separate, Windows-only fact bootstrap.ps1 DOES print (review item 7).
echo
info "Optional native libraries unlock the Yocto-side backends:"
case "${OS_LABEL}" in
    # HINT_*_NOTE is a bash ARRAY (one metadata/bootstrap.json
    # nativeLibHints.<os>.note entry per element) -- printed one line per
    # element below so the per-package mapping stays aligned instead of
    # collapsing into one unwrapped paragraph.  No indirect array expansion
    # (bash 3.2, the macOS-shipped version, has no namerefs) -- each case
    # duplicates the print loop instead.
    linux)
        echo
        for line in "${HINT_LINUX_NOTE[@]}"; do echo "  ${line}"; done
        HINT_CMD="${HINT_LINUX_CMD}"
        ;;
    macos)
        echo
        for line in "${HINT_MACOS_NOTE[@]}"; do echo "  ${line}"; done
        HINT_CMD="${HINT_MACOS_CMD}"
        ;;
    windows-bash)
        echo
        for line in "${HINT_WINDOWS_NOTE[@]}"; do echo "  ${line}"; done
        HINT_CMD="${HINT_WINDOWS_CMD}"
        ;;
    *)
        echo "  (OS not auto-detected; see docs/testing.md)"
        HINT_CMD=""
        ;;
esac
if [ -n "${HINT_CMD}" ]; then
    echo
    echo "  ${HINT_CMD}"
fi

# -------- Done ----------------------------------------------------------------

echo
ok "Bootstrap complete."
cat <<EOF

Next steps:
  # Activate the workspace venv (west + Zephyr/SDK deps live here):
  source "${VENV_DIR}/${VENV_POSIX_BIN}/activate"

  # Make Zephyr reachable for builds:
EOF
print_env_lines "  "
# Quoted heredoc tag (<<'EOF', not <<EOF): this block has no variable
# interpolation left to do, and an unquoted tag would run the backtick'd
# `cargo install ...` text below as a real command substitution instead of
# printing it literally -- caught during verification of this change (issue
# #917), pre-existing, unrelated to the manifest-consumption fix itself.
cat <<'EOF'

  # Sanity-check the host environment (needs tan on PATH -- see README.md
  # for `cargo install --git https://github.com/alplabai/tan-cli --bin tan`):
  tan doctor

  # Run the local test suite:
  bash scripts/test-all.sh

  # Or jump straight into building an example:
  west build -b native_sim/native/64 examples/peripheral-io/uart-echo \
      -- -DEXTRA_ZEPHYR_MODULES=$PWD

References:
  - docs/testing.md          -- full test-coverage map + how to run from scratch
  - docs/test-plan.md        -- per-feature verification ledger (⏳ / 🟡 / ✅)
EOF
