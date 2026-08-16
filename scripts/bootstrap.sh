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
DO_PATCHES=1
PRINT_ENV_ONLY=0
ALLOW_PARTIAL=0

while [ $# -gt 0 ]; do
    case "$1" in
        --no-pip)       DO_PIP=0 ;;
        --no-west)      DO_WEST=0 ;;
        --no-patches)   DO_PATCHES=0 ;;
        --print-env)    PRINT_ENV_ONLY=1 ;;
        --allow-partial) ALLOW_PARTIAL=1 ;;
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
    bash scripts/bootstrap.sh --no-patches   # skip `west patch apply` + its
        # verification (issue #1392). The patches in zephyr/patches.yml are
        # required to BUILD: zephyr/patches/zephyr/0002-ipm-add-poll-out-poll-in.patch
        # adds the ipm_driver_api .poll_out/.poll_in fields hal_alif's
        # se_service.c calls and the pinned upstream Zephyr does not have. Use
        # this only when you intend to manage the patch state yourself.
    bash scripts/bootstrap.sh --print-env    # only print env-var lines
    bash scripts/bootstrap.sh --allow-partial
        # report success even if zephyr-requirements / sdk-extras /
        # editable-install failed to install (issue #1038); the failures
        # are still printed and the workspace is left on disk either way --
        # this only changes the closing verdict

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

# Phase IDs (from metadata/bootstrap.json's verdict.blockingPhases, loaded
# further down) whose pip install actually failed THIS run -- a subset of
# VERDICT_BLOCKING_PHASES, filled in by record_phase_warning below as each
# pip step in "-------- pip dependencies --------" below completes. Read by
# the closing verdict logic in "-------- Done --------".
BLOCKING_PHASES=()
record_phase_warning() {
    # $1 = the phase id this warn() call just reported. Appends to
    # BLOCKING_PHASES only when VERDICT_BLOCKING_PHASES (metadata/
    # bootstrap.json, not hardcoded here) names it -- issue #1038:
    # pip-upgrade warns too (see the venv section below) but is deliberately
    # NOT in that list, so it never reaches this function.
    local phase="$1" p
    for p in "${VERDICT_BLOCKING_PHASES[@]}"; do
        if [ "${p}" = "${phase}" ]; then
            BLOCKING_PHASES+=("${phase}")
            return 0
        fi
    done
    return 0
}

# -------- Prerequisite check --------------------------------------------------

# Kept as ONE canonical hardcoded list (not read from the manifest: reading
# the manifest itself needs python3, one of these prerequisites --
# restructuring this into a manifest read would be a
# bootstrap-of-the-bootstrap). Its agreement with metadata/bootstrap.json's
# `prerequisites.posix` is policed by scripts/check_bootstrap_manifest.py,
# whose regex expects the full array assignment right below, not by this
# script.
REQUIRED_BINS=(git cmake python3 ninja xz wget)
# --print-env only reads metadata/bootstrap.json and prints -- it never
# touches git/cmake/ninja, so it only needs python3 present.
if [ "${PRINT_ENV_ONLY}" -eq 1 ]; then
    REQUIRED_BINS=(python3)
elif [ "$(uname -s 2>/dev/null)" = "Darwin" ]; then
    # xz and wget are REQUIRED on Linux, not macOS -- both are proven-on-Linux,
    # never-demonstrated-on-macOS failure modes, so blocking on them
    # unconditionally would hard-refuse every macOS user's first `bash
    # scripts/bootstrap.sh` -- the documented first command -- on a host
    # where it previously worked.
    #
    # xz: GNU tar (what `west sdk install`'s `tar --xz` shells out to on
    # Linux) execs a standalone `/usr/bin/xz` binary to unpack the SDK
    # archive -- the failure issue #949 actually proved, on a bare
    # ubuntu:24.04 container. macOS `tar` is bsdtar (libarchive), which
    # decompresses .xz IN-PROCESS and needs no xz binary, and stock macOS
    # ships no /usr/bin/xz.
    #
    # wget: the pinned Zephyr SDK's own `setup.sh` (metadata/toolchains.json
    # zephyrSdk.version) only hard-checks for a system `wget` on the
    # `linux-*` host branch (`[[ "${host}" =~ ^linux-.* ]] && check_command
    # wget 91`) -- confirmed by inspecting a real extracted setup.sh at that
    # exact pinned version. On `macos-*` it resolves a bundled wget from the
    # SDK's own hosttools instead and never runs that check at all -- issue
    # #949's clean-container acceptance run proved the Linux failure
    # (`Zephyr SDK setup requires 'wget'`), never a macOS one.
    #
    # `prerequisites.install.macos.{xz,wget}` (metadata/bootstrap.json) and
    # PREREQ_HINT_MACOS below stay as-is so the hint still exists for the
    # rare case a user needs either; only the hard block is dropped.
    REQUIRED_BINS=(git cmake python3 ninja)
fi

# Per-tool install hints for the missing-tools message below (issue #978) --
# the POSIX-side analogue of bootstrap.ps1's own $Prereqs Name/Hint pairs,
# and under the SAME bootstrap-of-the-bootstrap constraint as REQUIRED_BINS
# above (this runs before python3 is confirmed present, so it can't read
# metadata/bootstrap.json for the hint text either). A second hardcoded
# copy, kept in lockstep with metadata/bootstrap.json's
# prerequisites.install.linux.{apt,dnf} / .macos by
# scripts/check_bootstrap_manifest.py. Four PARALLEL arrays (was three --
# issue #1464 split the old single Linux array by PACKAGE MANAGER, not
# distro), not an associative array: bash 3.2 (the macOS-shipped version)
# has no `declare -A` -- the same reason the nativeLibHints print loop
# further down duplicates itself per OS instead of using indirection.
# Matched up by POSITION, not by key. PREREQ_HINT_DNF carries an EMPTY
# STRING for a tool metadata/bootstrap.json's install.linux.dnf doesn't
# declare (today: `ninja` -- Fedora's own repos carry `ninja-build`, but the
# RHEL-derivative default repos, measured on rockylinux:9, carry it under no
# name without EPEL, so this manifest ships no dnf.ninja rather than a
# guessed one) -- the empty-hint branch below already falls back to the bare
# tool name for that slot, the same host-neutral degrade a PM this script
# doesn't even try to detect (pacman -- deliberately unshipped, see
# metadata/schemas/bootstrap-v1.schema.json's install.linux description for
# why) already gets.
PREREQ_HINT_NAMES=(git cmake python3 ninja xz wget)
PREREQ_HINT_APT=(
    "sudo apt-get install -y git"
    "sudo apt-get install -y cmake"
    "sudo apt-get install -y python3"
    "sudo apt-get install -y ninja-build"
    "sudo apt-get install -y xz-utils"
    "sudo apt-get install -y wget"
)
PREREQ_HINT_DNF=(
    "sudo dnf install -y git"
    "sudo dnf install -y cmake"
    "sudo dnf install -y python3"
    ""
    "sudo dnf install -y xz"
    "sudo dnf install -y wget"
)
PREREQ_HINT_MACOS=(
    "brew install git"
    "brew install cmake"
    "brew install python3"
    "brew install ninja"
    "brew install xz"
    "brew install wget"
)

MISSING=()
for bin in "${REQUIRED_BINS[@]}"; do
    if ! command -v "${bin}" >/dev/null 2>&1; then
        MISSING+=("${bin}")
    fi
done
if [ "${#MISSING[@]}" -gt 0 ]; then
    warn "Missing required tools:"
    UNAME_S="$(uname -s 2>/dev/null || echo unknown)"
    # Package-manager detection (issue #1464) -- pure `command -v`, no
    # python3 needed (still bootstrap-of-the-bootstrap safe). Keyed by PM,
    # not distro: apt-get/dnf are each confirmable on an unknown host where
    # a distro ID is fuzzy (derivatives, stripped containers). pacman is
    # deliberately never probed here -- this manifest ships no
    # install.linux.pacman entry, so detecting it would only ever resolve to
    # an empty hint anyway; the bare tool name already prints in that case.
    LINUX_PM=""
    if [ "${UNAME_S}" != "Darwin" ]; then
        if command -v apt-get >/dev/null 2>&1; then
            LINUX_PM="apt"
        elif command -v dnf >/dev/null 2>&1; then
            LINUX_PM="dnf"
        fi
    fi
    for bin in "${MISSING[@]}"; do
        hint=""
        idx=0
        for name in "${PREREQ_HINT_NAMES[@]}"; do
            if [ "${name}" = "${bin}" ]; then
                case "${UNAME_S}" in
                    Darwin) hint="${PREREQ_HINT_MACOS[$idx]}" ;;
                    *)
                        case "${LINUX_PM}" in
                            apt) hint="${PREREQ_HINT_APT[$idx]}" ;;
                            dnf) hint="${PREREQ_HINT_DNF[$idx]}" ;;
                            *)   hint="" ;;
                        esac
                        ;;
                esac
                break
            fi
            idx=$((idx + 1))
        done
        if [ -n "${hint}" ]; then
            warn "  ${bin}  ->  ${hint}"
        else
            warn "  ${bin}"
        fi
    done
    die "Install the tools above and re-run."
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
# verdict: the single source for which pip phases make the closing verdict
# non-success, and the wording for it (issue #1038 / tan-cli#220) -- see
# the verdict.* descriptions in metadata/schemas/bootstrap-v1.schema.json.
# NOTE: no apostrophes, and no lone angle-bracket character, anywhere in
# this heredoc. bash 3.2 (macOS) scans this whole body -- comments included
# -- character by character while hunting for the closing paren of the
# enclosing $( ), even though the quoted PY-tagged heredoc makes it inert
# Python-only text to every other shell. An ODD apostrophe count opens a
# quote state that runs until the parser hits something illegal in it
# (#1050); a lone angle bracket (as in an angle-bracket-wrapped placeholder)
# is misread as a redirection operator and desyncs the same scan -- both
# surface as a syntax error at an unrelated line far below. Spell a
# placeholder as plain "user", not a bracketed one, down here.
# See #1050 and #1061.
emit("VERDICT_BLOCKING_PHASES", d["verdict"]["blockingPhases"])
emit("VERDICT_PARTIAL_NOTE_TEMPLATE", d["verdict"]["partialNoteTemplate"])
emit("VERDICT_INCOMPLETE_MESSAGE_TEMPLATE", d["verdict"]["incompleteMessageTemplate"])
emit("VERDICT_INCOMPLETE_REMEDY", d["verdict"]["incompleteRemedy"])
# env: keys and RAW (untokenized) values as two parallel arrays (bash has
# no portable ordered-map array type across the bash 3.2 macOS ships and
# bash 4+). Token substitution happens in bash, not here: git-bash silently
# rewrites a POSIX-style path argument (e.g. "/c/Users/user") into
# "C:/Users/user" when handed to a native (non-MSYS) python.exe, which would
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
# manualInstallHints.posix.note (issue #949 addendum A4): the Zephyr
# SDK / Arm GNU Toolchain manual-install facts, POSIX's twin of
# bootstrap.ps1's manualInstallHints.windows.note -- this key used to
# carry only "windows", so no Linux/macOS hint could ever render here.
emit("MANUAL_INSTALL_POSIX_NOTE", d["manualInstallHints"]["posix"]["note"])
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

# Intel Mac: this script's own steps (west init/update, Python deps) work
# fine -- they're arch-independent -- and so does native_sim (host-toolchain
# build, no Zephyr SDK involved). What does NOT work is `west sdk install`
# for real-silicon builds: the pinned Zephyr SDK (metadata/toolchains.json)
# ships macos-aarch64 only, no macos-x86_64 -- dropped upstream starting
# sdk-ng v1.0.0 (see docs/adr/0012-cross-platform-developer-host.md's
# 2026-07-29 Amendment, and the cross-platform setup guide's section 1).
# Warn, don't refuse: bootstrap itself and native_sim both still work here.
#
# `uname -m` alone is not enough: an Apple Silicon Mac running this
# script under Rosetta 2 (e.g. an x86_64 shell/terminal) also reports
# "x86_64", which would wrongly warn a native macos-aarch64 host.
# `sysctl -n sysctl.proc_translated` is the canonical discriminator --
# "1" means the CURRENT process is translated, "0" means it is native;
# the sysctl itself is macOS-only (absent on Linux, irrelevant there).
IS_ROSETTA=0
if [ "${OS_LABEL}" = "macos" ] && [ "$(sysctl -n sysctl.proc_translated 2>/dev/null || echo 0)" = "1" ]; then
    IS_ROSETTA=1
fi
if [ "${OS_LABEL}" = "macos" ] && [ "$(uname -m)" = "x86_64" ] && [ "${IS_ROSETTA}" -eq 0 ]; then
    warn "Intel Mac detected: native_sim and this bootstrap work fine, but"
    warn "  'west sdk install' will fail later -- the pinned Zephyr SDK ships"
    warn "  no macos-x86_64 build. Real-silicon Zephyr builds and 'tan build'"
    warn "  need a Linux host instead (VM, container, or remote builder)."
fi

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

# Refuse an empty or path-unsafe venv.dirName BEFORE it is ever used to
# build a delete target: an empty value collapses VENV_DIR to
# "${WORKSPACE_DIR}/", which the recreate-on-broken-venv path below then
# `rm -rf`s -- taking out the whole workspace (zephyr/, modules/, .west/,
# and the alp-sdk checkout itself if WORKSPACE_DIR is a parent of it).
# metadata/schemas/bootstrap-v1.schema.json constrains this same field, but
# a schema-drifted or hand-edited manifest must not reach the `rm -rf`
# unchecked -- this is the last line of defense, not a duplicate of that
# one.
case "${VENV_DIR_NAME}" in
    ""|.|..|*/*) die "metadata/bootstrap.json's venv.dirName is empty or path-unsafe (\"${VENV_DIR_NAME}\") -- refusing to build a workspace path (and a future 'rm -rf' target) from it." ;;
esac

# -------- workspace venv (hermetic west + Python deps) ------------------------

# Everything -- west, the Zephyr requirements, the SDK extras -- installs into a
# workspace-local venv, never the system interpreter / --user / --break-system-
# packages (issue #93: a half-removed system `packaging` once broke `west init`,
# and a global west couples the build to the host interpreter's state).
# alp-sdk's internal Python tooling + the VS Code extension auto-discover
# <workspace>/.venv, so this is backwards-compatible.  Idempotent: an
# existing WORKING venv is reused.
if [ "${DO_WEST}" -eq 1 ] || [ "${DO_PIP}" -eq 1 ]; then
    mkdir -p "${WORKSPACE_DIR}"
    # Reuse only a working venv (issue #985): `python3 -m venv` on a host
    # missing python3-venv creates the directory tree and a `python`
    # executable before failing at ensurepip, so the previous
    # executable-exists-only probe treated that half-built, pip-less venv
    # as valid and reused it forever -- every retry then died one step
    # later on "No module named pip" with nothing pointing at the venv
    # itself as the problem.  Probe the thing that actually broke: pip.
    VENV_REUSABLE=0
    for candidate in "${VENV_DIR}/${VENV_POSIX_BIN}/python" "${VENV_DIR}/${VENV_WINDOWS_BIN}/python.exe"; do
        if [ -x "${candidate}" ] && "${candidate}" -m pip --version >/dev/null 2>&1; then
            VENV_REUSABLE=1
            break
        fi
    done
    if [ "${VENV_REUSABLE}" -eq 1 ]; then
        ok "Workspace venv already present at ${VENV_DIR}"
    else
        if [ -e "${VENV_DIR}" ]; then
            warn "Workspace venv at ${VENV_DIR} exists but has no working pip -- recreating it"
            rm -rf "${VENV_DIR}"
        fi
        # -------- venv capability check (issue #984) -------------------------
        # `python3-venv` is a Debian/Ubuntu PACKAGE name, not a binary -- it
        # can never be a `command -v`-checkable REQUIRED_BINS entry (see that
        # array's comment above).  What actually breaks -- Debian/Ubuntu ship
        # the `ensurepip`-bundled pip/setuptools wheels `python3 -m venv`
        # needs in the separate `python3-venv` package, not in `python3`
        # itself -- only shows up when a venv is actually built, so `import
        # ensurepip` alone (importable even when the wheels are missing)
        # would not catch it.  This is a CAPABILITY probe -- build a
        # disposable throwaway venv and discard it -- not a presence probe,
        # using the same curated warn/die treatment the missing-tools message
        # above got in #978.  Runs only here, right before an actual venv
        # build is about to happen (issue #985 review): `--no-pip --no-west`
        # needs no venv at all, and an idempotent re-run of an already-healthy
        # venv was otherwise paying a real `python3 -m venv` (~1.65s measured)
        # for a probe nothing needed.
        VENV_PROBE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/alp-bootstrap-venv-probe.XXXXXX" 2>/dev/null || true)"
        if [ -z "${VENV_PROBE_DIR}" ]; then
            # mktemp itself failing (full/read-only/noexec TMPDIR, routine on
            # CI runners) is a DIFFERENT failure than python3-venv being
            # missing (checked next) and must not share that message: telling
            # the customer to install an already-installed package leaves
            # them stuck on the identical failure after they do -- the #985
            # misdiagnosis loop, reintroduced.
            die "mktemp -d under \${TMPDIR:-/tmp} (${TMPDIR:-/tmp}) failed -- check free space and write permissions there, or set TMPDIR to a writable directory, and re-run."
        fi
        if ! python3 -m venv "${VENV_PROBE_DIR}" >/dev/null 2>&1; then
            rm -rf "${VENV_PROBE_DIR}" 2>/dev/null || true
            warn "python3 -m venv is not usable (ensurepip unavailable):"
            case "${OS_LABEL}" in
                macos) warn "  python3-venv  ->  brew install python3" ;;
                *)     warn "  python3-venv  ->  sudo apt-get install -y python3-venv" ;;
            esac
            die "Install the package above and re-run."
        fi
        rm -rf "${VENV_PROBE_DIR}"
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
        info "Running 'west update' (shallow + narrow; ~1.5 GB+ on a cold cache for zephyr/ + modules/, mostly vendor HALs -- this is a floor, not a ceiling; budget disk/bandwidth accordingly)"
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
            || { warn "Zephyr requirements install reported a problem -- check manually"; record_phase_warning "zephyr-requirements"; }
    fi
    # SDK-side extras: alp_project.py needs jsonschema; the MCUboot
    # dev-key script needs imgtool.
    info "Installing alp-sdk Python extras into the venv (${PIP_SDK_EXTRAS[*]})"
    "${VPY}" -m pip install -q "${PIP_SDK_EXTRAS[@]}" \
        || { warn "alp-sdk extras install reported a problem -- check manually"; record_phase_warning "sdk-extras"; }
    # SDK-internal/reference Python tooling (including alp_cli) -- editable
    # install, so a `git pull` in the checkout updates it in place. Python
    # Tan is installed separately. During the v0.5 port, use tan-cli/dev in
    # its own Python 3.12+ venv; from v0.5 the installer supplies the frozen
    # runtime.
    info "Installing alp-sdk's internal Python tooling into the venv (pip install -e ${PIP_EDITABLE_INSTALL})"
    "${VPY}" -m pip install -q -e "${PIP_EDITABLE_INSTALL}" \
        || { warn "alp_cli editable install reported a problem -- check manually"; record_phase_warning "editable-install"; }
else
    info "Skipping pip installs (--no-pip)"
fi

# -------- zephyr/patches.yml (issue #1392) ------------------------------------

# AFTER the pip section, not inside the west one: `west patch` imports
# `pykwalify.core` at module import time, and pykwalify arrives with the Zephyr
# requirements installed just above. Run from the west block this exits
# non-zero in ~23 ms on a fresh CI workspace, before doing any work --
# `[bootstrap] west patch apply failed` across every alp-build matrix leg on
# PR #1426, with no output of its own to say why.
#
# This used to be a manual step nothing here mentioned, documented only in
# docs/aen-bench-bringup.md and replicated by pr-twister-aen.yml in its own
# stanza. A user who ran this script and started building got an unpatched
# tree, and every layer stayed quiet about it: bootstrap succeeded, the build
# succeeded, the flash succeeded, and the board did not boot the application.
#
# VERIFY FIRST, then apply only what is missing. `west patch apply` is NOT
# idempotent: re-running it on an already-patched tree fails, because each
# patch is fed to `git apply` against content that already carries it --
#
#   ERROR: error: patch failed: drivers/clock_control/clock_control_alif.c:124
#   error: drivers/clock_control/clock_control_alif.c: patch does not apply
#   FATAL ERROR: failed to apply patch zephyr/0001-clock_control_alif-...patch
#
# -- measured on PR #1426's `getting-started` job, whose
# `actions/cache@v5` key (`getting-started-aen801-zephyr-v4.4.1-${runner.os}`)
# carries no commit component, so it restored a `zephyr/`+`modules/` tree an
# earlier run had already patched. `scripts/bootstrap.sh`'s own `REUSE_WS=1`
# path reaches the same state for a developer re-running it. All three
# zephyr/patches.yml patches DO apply to a pristine v4.4.1 (`git apply --check`
# rc=0 each), so the tree being non-pristine is the whole story.
if [ "${DO_WEST}" -eq 1 ] && [ "${DO_PATCHES}" -eq 1 ]; then
    # No `set +e`/`set -e` guard around either call: this script runs under
    # `set -uo pipefail` with errexit OFF (line 37), so a non-zero exit does
    # not end the script and $? is readable directly. Adding the guard would
    # switch errexit ON for everything below it.
    ( cd "${REPO_ROOT}" && "${VPY}" scripts/verify_west_patches.py --topdir "${WORKSPACE_DIR}" --west "${WEST}" >/dev/null 2>&1 )
    VERIFY_RC=$?
    if [ "${VERIFY_RC}" -eq 0 ]; then
        ok "zephyr/patches.yml already applied in ${WORKSPACE_DIR} -- nothing to do"
    else
        # PER MODULE, not the whole set. A workspace can be PARTIALLY patched:
        # pr-getting-started-aen801.yml caches `zephyr` and `modules` under a
        # key with no commit component, but NOT `bootloader/mcuboot`, so zephyr
        # arrives already patched while mcuboot arrives fresh. A bare
        # `west patch apply` then re-applies zephyr's and dies on the first one
        # (`patch does not apply`), because the command is not idempotent.
        UNAPPLIED=$( cd "${REPO_ROOT}" && "${VPY}" scripts/verify_west_patches.py \
            --topdir "${WORKSPACE_DIR}" --west "${WEST}" --list-unapplied 2>/dev/null )
        if [ -z "${UNAPPLIED}" ]; then
            die "verify_west_patches.py reported patches missing (exit ${VERIFY_RC}) but named no module -- re-run it directly to see why"
        fi
        for _mod in ${UNAPPLIED}; do
            # `--dst-module` is a flag of `west patch` ITSELF, not of its
            # `apply` SUBCOMMAND, so it goes BEFORE `apply`:
            #
            #   usage: west patch [-h] [-b DIR] [-l FILE] [-w DIR] [-sm MODULE]
            #                     [-dm MODULE] <subcommand> ...
            #
            # Written the other way round it does not run at all --
            # `west patch: error: unexpected arguments: ['--dst-module', 'mcuboot']`,
            # exit 2 -- which failed every alp-build job on dev.
            info "Applying zephyr/patches.yml for module '${_mod}' ('west patch --dst-module ... apply')"
            PATCH_OUT=$( cd "${WORKSPACE_DIR}" && "${WEST}" patch --dst-module "${_mod}" apply 2>&1 )
            PATCH_RC=$?
            printf '%s\n' "${PATCH_OUT}"
            if [ "${PATCH_RC}" -ne 0 ]; then
                die "west patch --dst-module ${_mod} apply failed (exit ${PATCH_RC}) -- output above"
            fi
        done
        # Re-verify: `west patch apply`'s own exit status is not evidence it
        # did anything (three no-op-and-exit-0 paths -- see
        # scripts/verify_west_patches.py). Exit 3 is "everything present is
        # patched, but a module this workspace does not carry could not be
        # checked" -- normal for a narrow workspace, so it warns rather than
        # dying. 1 and 2 are the real thing.
        ( cd "${REPO_ROOT}" && "${VPY}" scripts/verify_west_patches.py --topdir "${WORKSPACE_DIR}" --west "${WEST}" )
        VERIFY_RC=$?
        case "${VERIFY_RC}" in
            0) ok "zephyr/patches.yml verified applied in ${WORKSPACE_DIR}" ;;
            3) warn "some zephyr/patches.yml modules are not in this workspace -- see above" ;;
            *) die "zephyr/patches.yml is not applied in ${WORKSPACE_DIR} (#1392) -- see the list above" ;;
        esac
    fi
elif [ "${DO_WEST}" -eq 1 ]; then
    info "Skipping 'west patch apply' (--no-patches) -- zephyr/patches.yml is NOT applied"
fi

# -------- Optional native libs hint -------------------------------------------

# Rendered from metadata/bootstrap.json's `nativeLibHints` (issue #917) --
# not hardcoded here; edit the manifest to change this text. This script is
# the sole consumer of nativeLibHints today: bootstrap.ps1 has no "native
# libraries" heading of its own (native Windows never installs the
# Yocto-side native libs directly) -- see `manualInstallHints` below for the
# separate fact bootstrap.ps1 ALSO prints, under its own OS key (review
# item 7).
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

# -------- Manual-install hints -------------------------------------------------

# Rendered from metadata/bootstrap.json's `manualInstallHints.posix.note`
# (issue #949 addendum A4) -- not hardcoded here; edit the manifest to
# change this text.  bootstrap.ps1 prints the `windows` twin of this same
# key under an identical heading; POSIX had no equivalent key to read
# until this addendum, so a Linux/macOS customer never saw this at all.
# Only for true POSIX (linux/macos) -- a git-bash/MSYS invocation on native
# Windows is the unsupported combo the top-of-file header comment already
# points elsewhere (WSL2 or bootstrap.ps1), so it gets neither this section
# nor windows' bootstrap.ps1-only one.
case "${OS_LABEL}" in
    linux|macos)
        echo
        info "NOT auto-installed (manual, one-time):"
        for line in "${MANUAL_INSTALL_POSIX_NOTE[@]}"; do echo "  ${line}"; done
        ;;
esac

# -------- Done ----------------------------------------------------------------

echo
# The closing verdict (issue #1038 / tan-cli#220): every pip phase above
# stays non-fatal in itself -- the workspace is left on disk regardless of
# which packages failed -- but a run that hit one of
# metadata/bootstrap.json's verdict.blockingPhases has NOT produced an
# environment that can do what it was bootstrapped for, and must not report
# unqualified success. BLOCKING_PHASES is populated by record_phase_warning
# above; VERDICT_* comes from the manifest, not hardcoded here, so this
# wording has exactly one declaration shared with scripts/bootstrap.ps1
# (and, independently, tan-cli's WORKSPACE_BLOCKING).
#
# EXIT_CODE is set here but NOT acted on until the very end of this script
# (matching tan-cli's `verdict()`/`finish()` split): the Next steps block
# below -- including `tan doctor`, the tool that diagnoses exactly
# this kind of failure -- must still print on the incomplete path. Exiting
# here would take that away on the one run that needs it most.
EXIT_CODE=0
if [ "${#BLOCKING_PHASES[@]}" -eq 0 ]; then
    ok "Bootstrap complete."
elif [ "${ALLOW_PARTIAL}" -eq 1 ]; then
    _phases_joined=""
    for _p in "${BLOCKING_PHASES[@]}"; do
        if [ -z "${_phases_joined}" ]; then _phases_joined="${_p}"; else _phases_joined="${_phases_joined}, ${_p}"; fi
    done
    ok "Bootstrap complete."
    warn "  ${VERDICT_PARTIAL_NOTE_TEMPLATE//\{\{PHASES\}\}/${_phases_joined}}"
else
    _phases_joined=""
    for _p in "${BLOCKING_PHASES[@]}"; do
        if [ -z "${_phases_joined}" ]; then _phases_joined="${_p}"; else _phases_joined="${_phases_joined}, ${_p}"; fi
    done
    warn "${VERDICT_INCOMPLETE_MESSAGE_TEMPLATE//\{\{PHASES\}\}/${_phases_joined}}"
    warn "  ${VERDICT_INCOMPLETE_REMEDY}"
    EXIT_CODE=1
fi
# Split into two heredocs on purpose.  Only this first block has variables
# left to expand, so only it gets an UNQUOTED tag.
cat <<EOF

Next steps:
  # Activate the workspace venv (west + Zephyr/SDK deps live here):
  source "${VENV_DIR}/${VENV_POSIX_BIN}/activate"

  # Make Zephyr reachable for builds:
EOF
# The env lines dev used to hardcode here are now rendered from
# metadata/bootstrap.json's `env` map (issue #917), so they stay in one
# place for both this script and bootstrap.ps1.
print_env_lines "  "
# QUOTED tag (<<'EOF') -- mandatory, not stylistic.  This block documents
# shell commands, and an unquoted tag makes the shell treat the backtick'd
# install command below as a real command SUBSTITUTION: every completed
# run of this script silently executed it, reinstalling tan behind the
# user's back, and pasted its output here instead of the text.  A quoted
# tag also means backslashes are literal, so the line continuation and
# $PWD below are written plainly rather than escaped.
#
# Everything between the EOF markers is PRINTED TO THE USER, so a '#' line in
# there is output, not a source comment -- keep internal notes (like this one)
# outside the heredoc. `check_bootstrap_manifest.py` enforces exactly that
# distinction, and `tests/scripts/test_check_bootstrap_manifest.py`
# ::test_hardcoded_literal_inside_heredoc_body_fails proves it by rewriting the
# printed `  # Run the local test suite:` line to carry a version literal and
# asserting the gate goes red. That test asserts its anchor exists and fails
# loudly if it does not, so rewording that printed line breaks a real gate for
# a non-reason. Reword the lines around it instead.
cat <<'EOF'

  # Sanity-check the host build environment (needs tan on PATH -- see
  # README.md for the current v0.5-transition install). Python Tan runs one
  # build/flash-oriented checklist; `--build` is a compatibility no-op.
  tan doctor

  # BUILDING YOUR OWN PROJECT -- the customer path. `tan` is the whole command
  # surface (ADR-0020), and `tan build` resolves the board from the project's
  # own board.yaml, so there is no -b to pass. `tan examples` lists what you
  # can start from.
  #
  # Note `tan build` has NO native_sim option: board.yaml's `os:` is
  # zephyr/yocto/baremetal/off, so it always targets the real SKU your
  # board.yaml declares and a real toolchain is required. `tan doctor`
  # reports whether you have one.
  tan init --name my-app --destination .. --sdk-root "$PWD"
  cd ../my-app && tan build

  # WORKING ON THE SDK ITSELF -- contributor commands, not part of building
  # your firmware. native_sim is reachable only through west, for the same
  # reason as above: it is not a SKU any board.yaml can declare.
  west build -b native_sim/native/64 examples/peripheral-io/uart-echo       -- -DEXTRA_ZEPHYR_MODULES=$PWD

  # Run the local test suite:
  bash scripts/test-all.sh

References:
  - docs/testing.md          -- full test-coverage map + how to run from scratch
  - docs/test-plan.md        -- per-feature verification ledger (⏳ / 🟡 / ✅)
EOF

# EXIT_CODE was decided by the closing verdict above (1 only on the
# INCOMPLETE path, i.e. --allow-partial was not passed) -- deferred until
# here so the Next steps block always prints first, on every path (issue
# #1038 / tan-cli#220).
exit "${EXIT_CODE}"
