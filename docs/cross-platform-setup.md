# Cross-platform developer setup

The Alp SDK supports development on **Linux, macOS, native
Windows, and Windows + WSL2**.  The Zephyr-on-M-class workflow
(`board.yaml` → `west build` → flash) is first-class on every
host OS — same source tree, same `west.yml`, same metadata,
identical artefacts — with one exception: real-silicon builds on
macOS need Apple Silicon (see §1, §3).

The Yocto-on-A-class workflow is Linux-only by upstream constraint
(`bitbake` does not run on macOS or native Windows).  Mac /
Windows users targeting Yocto reach for WSL2 or a Linux VM.

This guide is the long-form reference.  For the fast path see
[`docs/getting-started.md`](getting-started.md); for the rationale
see [ADR 0012](adr/0012-cross-platform-developer-host.md).

## Contents

Sections:

- §1 Overview -- workflow vs host matrix
- §2 Linux setup (Debian / Ubuntu / Fedora)
- §3 macOS setup (13 Ventura+; real silicon is Apple Silicon only)
- §4 Windows native setup (PowerShell)
- §5 Windows + WSL2 setup (for Yocto targets)
- §6 Verification -- hello-world per OS
- §7 Known gotchas
- §8 What is Linux-only and why

---

## 1. Overview — workflow vs host matrix

The SDK exposes four developer workflows.  Each row below shows
which host OSes support which workflow natively.  **Native** means
"runs directly on the host without a Linux emulation layer";
**WSL2 / VM** means "runs inside a Linux environment hosted on
the OS".

| Workflow | Linux | macOS | Win native | Win + WSL2 |
|---|---|---|---|---|
| **Zephyr-on-M (`native_sim`)** — examples, ztests, day-to-day iteration | yes | yes | no (use WSL2) | yes (via WSL) |
| **Zephyr-on-M (real silicon)** — `west build` + `west flash` against EVK | yes | **Apple silicon only** | yes | yes (via WSL) |
| **Yocto-on-A (`bitbake`)** — full Linux userland image build | yes | no | no | yes |
| **Heterogeneous orchestrator (`tan build` fanning out across cores)** | yes | **Apple silicon only** (Zephyr halves) | yes (Zephyr halves only) | yes |

Read: a Mac user can do everything on the host **except** build
the Yocto half (use a Linux VM for that).  A Windows user runs the
cross-compiled real-silicon `west build` natively in PowerShell,
but **both** the `native_sim` simulator target and the Yocto half
need WSL2 — upstream Zephyr's `native_sim` is Linux/macOS only and
has no native-Windows target.  Switching is `cd {project}` from
PowerShell into the WSL filesystem
(`\\wsl$\Ubuntu-22.04\home\{user}\...`).

**Intel Macs cannot cross-compile.**  The two "Apple silicon only"
cells above are not a preference — the pinned Zephyr SDK
(`metadata/toolchains.json`, version `1.0.1`) publishes host builds
for `linux-x86_64`, `macos-aarch64` and `windows-x86_64` only
(`linux-aarch64` exists upstream at this release too, but the
manifest's own `_comment` records it as deliberately not listed —
no current alp-sdk path runs or downloads it).  Upstream shipped `macos-x86_64` through
`0.17.4` and dropped it in `1.0.0`, so there is no
`arm-zephyr-eabi` cross toolchain for an Intel Mac at the version
this SDK pins.  `macos-aarch64` is not a substitute: Rosetta
translates x86_64 *for* Apple silicon, not the reverse, and macOS
has no WSL2 equivalent to fall back to.  What an Intel Mac *can*
still do is everything that uses the host compiler — `native_sim`
builds, ztests, `tan validate`, `tan doctor`, metadata work — and
`tan` itself has a published `tan-x86_64-apple-darwin.tar.gz` release archive
and runs fine. For real-silicon firmware, build on a Linux host.

ADR [0012](adr/0012-cross-platform-developer-host.md) is the
load-bearing decision behind this matrix.  ADR
[0010](adr/0010-heterogeneous-os-orchestration.md) is why
heterogeneous orchestration is its own row.

### 1.1 Python version: SDK floor, Tan floor, and Zephyr floor

Every host workflow needs Python. Three contracts matter:

- **Support floor: 3.10.**  `pyproject.toml` declares
  `requires-python = ">=3.10"` — the SDK's Python tooling
  (validators, orchestrator, metadata-only work such as `--emit`
  and direct SDK validation) runs on any 3.10+. This is also what
  `metadata/bootstrap.json`'s `prerequisites.pythonMinVersion`
  currently checks before `scripts/bootstrap.sh`/`scripts/bootstrap.ps1`
  use as the manifest floor.
- **Python Tan source floor: 3.12.** Release archives bundle their own Python
  runtime. A source install (`python3 -m pip install ./python`) needs 3.12+.
- **Dev/CI pin: 3.12.**  The repo-root `.python-version` file is
  the single source; every CI workflow's `actions/setup-python`
  reads it via `python-version-file`, so CI always runs exactly
  the pinned version.
- **Zephyr's own build floor: 3.12.** Zephyr v4.4.1's
  `cmake/modules/python.cmake` hardcodes
  `set(PYTHON_MINIMUM_REQUIRED 3.12)` and
  `find_package(Python3 3.12 REQUIRED)`, so a `west build`/CMake
  configure of this workspace refuses below 3.12 no matter what the
  support floor above accepted. The number is recorded separately as
  `metadata/bootstrap.json`'s
  `zephyr.pythonMinVersion` (issue #1078) — derived from, and checked
  against, the pinned Zephyr's own `cmake/modules/python.cmake` by
  `scripts/check_bootstrap_manifest.py`, so a future Zephyr bump that
  changes its own floor can't drift this field silently. Python Tan's
  `bootstrap` and `doctor` enforce the effective maximum of the SDK manifest
  and resolved Zephyr floors. Install a newer interpreter alongside the
  system one (`deadsnakes` PPA or `pyenv`) when necessary. See
  [`docs/troubleshooting.md`](troubleshooting.md)'s "`west build`
  fails at CMake configure citing a Python version".

To reproduce CI byte-for-byte, match the pin locally — `pyenv` and `uv` pick
`.python-version` up automatically.

### 1.2 Installing Tan from source

As of `tan-cli` [v0.5.0](https://github.com/alplabai/tan-cli/releases/tag/v0.5.0)
(current release: [v0.5.1](https://github.com/alplabai/tan-cli/releases/tag/v0.5.1)),
the published installer (`install.sh`/`install.ps1`) installs the real Python
`tan` directly -- it no longer resolves the frozen Rust v0.4.1 release.
alp-sdk `dev` tracks `tan-cli/dev` instead, to stay ahead of the last tagged
release; install the current Python source in an isolated Python 3.12+
environment:

```bash
git clone --branch dev https://github.com/alplabai/tan-cli
python3 -m venv tan-cli/.venv
tan-cli/.venv/bin/python -m pip install ./tan-cli/python
```

The release installer's PyInstaller archives bundle their own Python
runtime, so release users do not install Python separately. Alp's `tan`
is not distributed on PyPI, and the bare name `tan` there belongs to an
unrelated project (`200` for it: `tan` v23.7.0, "The compromising code
formatter") -- `pip install tan` does not get you this tool. `alp-tan` is
not registered there either (`404` for it, not a reservation placeholder).

---

## 2. Linux setup (Debian / Ubuntu / Fedora)

Linux is the canonical baseline.  All CI gates run on
`ubuntu-latest`.  If you're picking a distro to evaluate from
scratch, Ubuntu 22.04 LTS or 24.04 LTS will line up with the
shipped CI exactly.

### 2.1 Base toolchain

```bash
sudo apt update
sudo apt install -y \
    git python3 python3-pip python3-venv \
    cmake ninja-build gperf ccache \
    device-tree-compiler \
    build-essential file xz-utils wget \
    libffi-dev libssl-dev libsdl2-dev libmagic1 \
    dfu-util
```

For Fedora / RHEL the equivalent is:

```bash
sudo dnf install -y \
    git python3 python3-pip python3-virtualenv \
    cmake ninja-build gperf ccache \
    dtc \
    gcc gcc-c++ make file xz wget \
    libffi-devel openssl-devel SDL2-devel file-libs \
    dfu-util
```

### 2.2 Python deps

```bash
pip3 install --user west pyyaml jsonschema imgtool pytest
```

### 2.3 Arm GNU Toolchain (three opt-in paths, not the Zephyr-on-M default)

The Zephyr-on-M real-silicon path (`west build` + `west flash` against an
E1M / E1M-X EVK) does **not** need this toolchain -- it cross-compiles
with the Zephyr SDK's `arm-zephyr-eabi` (`west sdk install`;
`ZEPHYR_TOOLCHAIN_VARIANT=zephyr`).  For a manual sdk-ng install instead
of `west sdk install`, see [`docs/local-ci.md`](local-ci.md) Path A
step 4.
Ubuntu's own apt `gcc-arm-none-eabi` predates Cortex-M55/Helium support,
which is why CI installs the Zephyr SDK toolchain instead of this one
(`.github/workflows/pr-getting-started-aen801.yml`).

The Arm GNU Toolchain below is needed by three opt-in paths, none of
them the Zephyr-on-M default:

- Rebuilding the **E1M-X V2N / V2N-M1 GD32 bridge firmware** --
  pre-flashed by Alp Lab, so normal customers never touch it, but fully
  open to rebuild (see [`docs/gd32-bridge.md`](gd32-bridge.md)):
  custom-carrier bring-up ([`docs/bring-up-v2n.md`](bring-up-v2n.md)) or
  bridge recovery
  ([`docs/tutorials/07-recovering-a-bricked-bridge.md`](tutorials/07-recovering-a-bricked-bridge.md)).
- Building the **CC3501E bridge firmware's silicon-free stub target**
  ([`cc3501e-bridge-firmware:README.md`](https://github.com/alplabai/cc3501e-bridge-firmware#readme) "Build")
  -- the CC3501E's *production* image builds with TI's `ticlang`, not
  this toolchain
  ([`cc3501e-bridge-firmware:toolchain/arm-none-eabi.cmake`](https://github.com/alplabai/cc3501e-bridge-firmware));
  only the stub / CI-compile-smoke target needs `arm-none-eabi-gcc`.
- Hand-writing **bare-metal firmware for a real M-class core**
  (`ALP_OS=baremetal`, no Zephyr -- see [`docs/architecture.md`](architecture.md)
  "OS targets").  Caveat: the SDK does not ship a cross-compiled
  bare-metal build recipe today -- the in-repo `ALP_OS=baremetal` CI job
  (`.github/workflows/pr-plain-cmake.yml`) compiles with the **host**
  toolchain as a compile smoke, not against real M-class silicon.  A
  hand-written bare-metal firmware targeting real silicon needs this
  toolchain; wiring the cross-compile yourself is on you until a recipe
  ships.

```bash
# Download the 13.x release from arm.com (no apt package tracks
# the version policy in docs/zephyr-version-policy.md).
ARM_GNU_VER="13.3.rel1"
curl -L "https://developer.arm.com/-/media/Files/downloads/gnu/${ARM_GNU_VER}/binrel/arm-gnu-toolchain-${ARM_GNU_VER}-x86_64-arm-none-eabi.tar.xz" \
    -o arm-gnu.tar.xz
sudo mkdir -p /opt/arm-gnu
sudo tar -xJf arm-gnu.tar.xz -C /opt/arm-gnu --strip-components=1
echo 'export PATH="/opt/arm-gnu/bin:$PATH"' >> ~/.bashrc
```

(Restart your shell or `source ~/.bashrc` after editing.)

### 2.4 Optional Yocto deps

You only need these if you're targeting the Yocto-on-A half of an
E1M-X SoM.  See [`docs/heterogeneous-builds.md`](heterogeneous-builds.md)
for the full bitbake walkthrough.

```bash
# Yocto host build deps per the Yocto Reference Manual.
sudo apt install -y \
    gawk wget diffstat unzip texinfo chrpath socat cpio \
    python3-distutils python3-jinja2 \
    iputils-ping libegl1-mesa libsdl1.2-dev xterm zstd liblz4-tool

# Yocto build uses meta-alp-sdk's bitbake-layers flow
# (see meta-alp-sdk/README.md) -- no kas/front-end needed.
```

### 2.5 west workspace

Pick a parent directory for the workspace; this can be anywhere
(`~/dev/`, `~/projects/`, etc.):

```bash
mkdir -p ~/dev/alp-workspace && cd ~/dev/alp-workspace
west init -m https://github.com/alplabai/alp-sdk
west update --narrow -o=--depth=1
west zephyr-export
export ZEPHYR_BASE="$PWD/zephyr"
```

Persist `ZEPHYR_BASE` in your shell profile (typically
`~/.bashrc` on Linux; on Fedora some setups use `~/.bash_profile`).

---

## 3. macOS setup (13 Ventura+)

macOS is supported natively for the Zephyr-on-M `native_sim`
workflow on both architectures; real-silicon builds are Apple
Silicon only (see §1).  Mac users targeting Yocto reach for a
Linux VM (UTM, Parallels, VirtualBox) — `bitbake` does not run on
macOS.

### 3.1 Homebrew prerequisites

If Homebrew is not already installed:

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

### 3.2 Base toolchain

```bash
brew install \
    git python@3.12 \
    cmake ninja gperf ccache \
    dtc \
    libffi openssl sdl2 \
    dfu-util
```

`brew install python@3.12` puts `python3` + `pip3` on the path.

### 3.3 Python deps

```bash
pip3 install --user west pyyaml jsonschema imgtool pytest
```

### 3.4 Arm GNU Toolchain (three opt-in paths, not the Zephyr-on-M default)

Same scoping as §2.3: the Zephyr-on-M real-silicon path uses the Zephyr
SDK's `arm-zephyr-eabi` (`west sdk install`), not this toolchain.  The
Arm GNU Toolchain below is needed by three opt-in paths, none of them
the Zephyr-on-M default:

- Rebuilding the **E1M-X V2N / V2N-M1 GD32 bridge firmware** --
  pre-flashed by Alp Lab and optional to touch, but fully open (see
  [`docs/gd32-bridge.md`](gd32-bridge.md)): custom-carrier bring-up
  ([`docs/bring-up-v2n.md`](bring-up-v2n.md)) or bridge recovery
  ([`docs/tutorials/07-recovering-a-bricked-bridge.md`](tutorials/07-recovering-a-bricked-bridge.md)).
- Building the **CC3501E bridge firmware's silicon-free stub target**
  ([`cc3501e-bridge-firmware:README.md`](https://github.com/alplabai/cc3501e-bridge-firmware#readme) "Build")
  -- the CC3501E's *production* image builds with TI's `ticlang`, not
  this toolchain; only the stub / CI-compile-smoke target needs
  `arm-none-eabi-gcc`.
- Hand-writing **bare-metal firmware for a real M-class core**
  (`ALP_OS=baremetal`, no Zephyr).  Caveat: the SDK does not ship a
  cross-compiled bare-metal build recipe today -- the in-repo
  `ALP_OS=baremetal` CI job (`.github/workflows/pr-plain-cmake.yml`)
  compiles with the **host** toolchain as a compile smoke, not against
  real M-class silicon.  A hand-written bare-metal firmware targeting
  real silicon needs this toolchain; wiring the cross-compile yourself
  is on you until a recipe ships.

```bash
brew install --cask gcc-arm-embedded
# Confirm:
arm-none-eabi-gcc --version
```

If you prefer the arm.com installer:

```bash
ARM_GNU_VER="13.3.rel1"
curl -L "https://developer.arm.com/-/media/Files/downloads/gnu/${ARM_GNU_VER}/binrel/arm-gnu-toolchain-${ARM_GNU_VER}-darwin-x86_64-arm-none-eabi.tar.xz" \
    -o arm-gnu.tar.xz
sudo mkdir -p /opt/arm-gnu
sudo tar -xJf arm-gnu.tar.xz -C /opt/arm-gnu --strip-components=1
```

For Apple Silicon (M1/M2/M3) macs, swap `darwin-x86_64` for
`darwin-arm64` in the URL above.  Add `/opt/arm-gnu/bin` to your
`PATH`.

### 3.5 Shell profile notes

macOS Catalina (10.15) and later default to **zsh**, not bash.
Put environment lines in `~/.zshrc`, not `~/.bashrc`:

```zsh
# ~/.zshrc
export PATH="/opt/arm-gnu/bin:$PATH"
export ZEPHYR_BASE="$HOME/dev/alp-workspace/zephyr"
```

If you have explicitly switched the login shell back to bash
(`chsh -s /bin/bash`), use `~/.bash_profile` instead.

### 3.6 west workspace

Same as Linux:

```bash
mkdir -p ~/dev/alp-workspace && cd ~/dev/alp-workspace
west init -m https://github.com/alplabai/alp-sdk
west update --narrow -o=--depth=1
west zephyr-export
export ZEPHYR_BASE="$PWD/zephyr"
```

### 3.7 Optional: Linux VM for Yocto

If your project needs the Yocto half built on the same Mac, the
cleanest path is UTM (free) running Ubuntu 22.04:

```bash
brew install --cask utm
# Then download an Ubuntu 22.04 server ISO and follow the UTM
# walkthrough.
```

Inside the VM, follow §2 above.  Map the alp-sdk repo as a
shared folder so you can edit on macOS and bitbake on Linux
against the same source.

---

## 4. Windows native setup (PowerShell)

Native Windows is supported for the Zephyr-on-M workflow.
Windows users targeting Yocto see §5 (WSL2) below.

The examples in this section use **PowerShell 7+** syntax.  Open
PowerShell as Administrator for the first-time install steps; the
day-to-day workflow runs as a normal user.

> **Shortcut:** `pwsh scripts\bootstrap.ps1` automates §4.2 + §4.5
> (venv + Python deps + west workspace; `tan` itself is a standalone
> binary set up via `tan bootstrap`) once the
> §4.1 base toolchain is present — it prints the matching `winget`
> one-liner for anything missing and is idempotent.  The Arm GNU
> Toolchain (§4.3) and Zephyr SDK stay manual (GUI installers).  The
> sections below remain the manual walkthrough the script automates.
> Both this script and its Linux/macOS twin `scripts/bootstrap.sh` read
> their facts (the pinned Zephyr version, venv layout, `west`/`pip`
> arguments, optional-native-lib hints) from
> [`metadata/bootstrap.json`](../metadata/bootstrap.json) -- a single
> source both scripts stay in lockstep with, policed by
> `scripts/check_bootstrap_manifest.py` (see
> [`docs/zephyr-version-policy.md`](zephyr-version-policy.md)).

### 4.1 Base toolchain via winget

```powershell
winget install -e --id Git.Git
winget install -e --id Python.Python.3.12
winget install -e --id Kitware.CMake
winget install -e --id Ninja-build.Ninja
```

Close and reopen PowerShell after the installs so the updated
`PATH` is picked up.

Two more tools, listed separately because they are build-time and
optional rather than part of the four required above:

```powershell
winget install -e --id oss-winget.dtc
winget install -e --id oss-winget.gperf
```

Neither `dtc` nor `gperf` is in `prerequisites.windows`, and
`bootstrap.ps1` does not require them. `tan doctor`'s checks for both are
WARN-only: `edtlib` does the load-bearing devicetree parse in pure Python (a
missing `dtc` never blocks a build), and plain kernel-mode apps build without
`gperf`.  Install them if your build needs extra dts validation or
kobject/userspace generation -- the Zephyr SDK's Windows bundle ships
neither.

### 4.2 Python deps

```powershell
pip install --user west pyyaml jsonschema imgtool pytest
```

If `pip` is not on PATH after the Python install, run
`python -m ensurepip --upgrade` and `python -m pip install --user west ...`.

### 4.3 Arm GNU Toolchain (three opt-in paths, not the Zephyr-on-M default)

Same scoping as §2.3: the Zephyr-on-M real-silicon path uses the Zephyr
SDK's `arm-zephyr-eabi` (`west sdk install`), not this toolchain.  For a
manual sdk-ng install instead of `west sdk install`, see
[`docs/local-ci.md`](local-ci.md) Path B step 5 (the Windows Zephyr-SDK
walkthrough).  The Arm GNU Toolchain below is needed by three opt-in
paths, none of them the Zephyr-on-M default:

- Rebuilding the **E1M-X V2N / V2N-M1 GD32 bridge firmware** --
  pre-flashed by Alp Lab and optional to touch, but fully open (see
  [`docs/gd32-bridge.md`](gd32-bridge.md)): custom-carrier bring-up
  ([`docs/bring-up-v2n.md`](bring-up-v2n.md)) or bridge recovery
  ([`docs/tutorials/07-recovering-a-bricked-bridge.md`](tutorials/07-recovering-a-bricked-bridge.md)).
- Building the **CC3501E bridge firmware's silicon-free stub target**
  ([`cc3501e-bridge-firmware:README.md`](https://github.com/alplabai/cc3501e-bridge-firmware#readme) "Build")
  -- the CC3501E's *production* image builds with TI's `ticlang`, not
  this toolchain; only the stub / CI-compile-smoke target needs
  `arm-none-eabi-gcc`.
- Hand-writing **bare-metal firmware for a real M-class core**
  (`ALP_OS=baremetal`, no Zephyr).  Caveat: the SDK does not ship a
  cross-compiled bare-metal build recipe today -- the in-repo
  `ALP_OS=baremetal` CI job (`.github/workflows/pr-plain-cmake.yml`)
  compiles with the **host** toolchain as a compile smoke, not against
  real M-class silicon.  A hand-written bare-metal firmware targeting
  real silicon needs this toolchain; wiring the cross-compile yourself
  is on you until a recipe ships.

The arm.com installer ships a Windows MSI:

```powershell
# Open the page, download the Windows installer (.exe), and run it:
Start-Process "https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads"
```

Pick `arm-gnu-toolchain-13.3.rel1-mingw-w64-i686-arm-none-eabi.exe`
(or whatever the current 13.x release is per
`docs/zephyr-version-policy.md`).  Tick "Add path to environment
variable" during install.

Verify:

```powershell
arm-none-eabi-gcc --version
```

### 4.4 Persist environment variables

Two ways to set `ZEPHYR_BASE` (and any other env vars) persistently
on Windows:

**Option A — Setx (PowerShell, user-scoped, persists across
sessions):**

```powershell
setx ZEPHYR_BASE "$HOME\dev\alp-workspace\zephyr"
# Restart PowerShell for the new env var to take effect.
```

**Option B — System Properties → Environment Variables GUI**
(Win+R → `sysdm.cpl` → Advanced tab → Environment Variables).
Both approaches edit the same registry-backed user environment.

For the *current* shell only (does not persist):

```powershell
$env:ZEPHYR_BASE = "$HOME\dev\alp-workspace\zephyr"
```

There is **no Windows equivalent of `~/.bashrc`**.  PowerShell
profile scripts (`$PROFILE`) get close but are not loaded by every
shell host.  Use `setx` for things that should be globally set.

### 4.5 west workspace

```powershell
mkdir $HOME\dev\alp-workspace
cd $HOME\dev\alp-workspace
west init -m https://github.com/alplabai/alp-sdk
west update --narrow -o=--depth=1
west zephyr-export
$env:ZEPHYR_BASE = "$PWD\zephyr"
```

### 4.6 Native_sim on Windows — use WSL2

Zephyr's `native_sim` Posix target builds as a **native Linux
process**.  Upstream Zephyr supports `native_sim` on Linux and
macOS only — there is no native-Windows `native_sim` target.  On
Windows, run `native_sim/native/64` inside WSL2 (Ubuntu); see
[`docs/local-ci.md`](local-ci.md) "Path A — WSL2".

Cross-compiled real-silicon builds (`west build -b alp_e1m_aen801_m55_he/...
...`) run fine in native PowerShell — only the host-side
`native_sim` simulator target requires WSL2.

---

## 5. Windows + WSL2 setup (for Yocto targets)

WSL2 (Windows Subsystem for Linux v2) gives you a real Linux
kernel running alongside Windows.  This is the **only** path that
runs `bitbake` on a Windows host.

If you only need the Zephyr-on-M workflow, **skip this section**
— native PowerShell (§4) is faster and lighter.  WSL2 is only
required when you need Yocto.

### 5.1 Install WSL2

```powershell
# Run as Administrator.
wsl --install -d Ubuntu-22.04
# Reboot when prompted.  WSL2 backend is the default on Windows 11.
```

On Windows 10 builds <19041 you may need a manual two-step:
`wsl --set-default-version 2` followed by an Ubuntu install from
the Microsoft Store.

### 5.2 Bring up the Linux environment

Inside the WSL Ubuntu shell, follow §2 above — the apt
commands work as-is.  WSL2 ships with `systemd` enabled on
recent Ubuntu images.

### 5.3 Cross-filesystem editing

Two cross-edit patterns work well:

1. **Edit on Windows, build in WSL.**  Keep the alp-sdk
   workspace inside the WSL filesystem (e.g.
   `/home/{user}/dev/alp-workspace/`) for fast I/O.  Access from
   Windows tools (VS Code, Explorer) via a Windows UNC path
   pointing at the WSL distro's filesystem (template + concrete
   example below):

   ```text
   Template: \\wsl$\{distro}\home\{user}\...
   Example : \\wsl$\Ubuntu-22.04\home\<user>\dev\alp-workspace
   ```

   VS Code with the *Remote -- WSL* extension handles this
   transparently.
2. **Edit in WSL, build in WSL.**  Open VS Code via
   `code .` inside the WSL shell — VS Code launches a remote
   session and your editor runs in WSL natively.

**Avoid**: keeping the workspace under `/mnt/c/Users/...`.  The
Windows-mounted DrvFs has ~10× slower file I/O than the WSL2
ext4 filesystem; bitbake parsing alone takes minutes instead
of seconds on DrvFs.

### 5.4 USB / serial passthrough

For flashing real hardware from inside WSL2, install
[usbipd-win](https://github.com/dorssel/usbipd-win):

```powershell
winget install -e --id dorssel.usbipd-win
```

Attach a USB device to WSL:

```powershell
# In an admin PowerShell:
usbipd list                                  # find the BUSID
usbipd bind --busid <BUSID>
usbipd attach --wsl --busid <BUSID>
```

Inside WSL:

```bash
lsusb   # device now visible
```

For the common case of "just flash the dev board from
Windows-native", the vendor flashers (J-Link, OpenOCD, Alif Conf
Tool) on Windows talk to the same USB device without WSL — you
only need usbipd-win if you're running the flasher inside WSL.

### 5.5 X11 / GUI tools

WSLg ships with Windows 11 and modern Windows 10 updates — GUI
apps inside WSL just work, no X server install needed.  Confirm:

```bash
sudo apt install -y x11-apps
xeyes &
```

If you're on an older Windows 10 build without WSLg, install
[VcXsrv](https://sourceforge.net/projects/vcxsrv/) and set
`export DISPLAY=$(grep nameserver /etc/resolv.conf | awk '{print $2}'):0`
in WSL.

---

## 6. Verification — hello-world per OS

After install, this is the minimum smoke test you can run on
every host to confirm the SDK is wired up.

### 6.1 Metadata validation (no Zephyr needed)

```bash
# Linux / macOS / WSL (bash or zsh):
cd alp-sdk
python3 scripts/validate_metadata.py
```

```powershell
# Windows native (PowerShell):
cd alp-sdk
python scripts\validate_metadata.py
```

Expected: exits 0, prints a per-SoC validation line.  This
exercises the pure-Python metadata layer — if this fails the
Python deps install was incomplete.

### 6.2 Cross-platform lint

```bash
python3 scripts/check_cross_platform.py
```

```powershell
python scripts\check_cross_platform.py
```

Expected: exits 0; may print warnings about Linux-only idioms in
docs.  These warnings are informational today (the lint is soft);
they document drift for future cleanup.  See
[ADR 0012](adr/0012-cross-platform-developer-host.md) for why the
lint is soft initially.

### 6.3 Native_sim example build

This is the cross-platform end-to-end smoke test. `tan build --project ...`
follows `gpio-button-led`'s `board.yaml`, which targets a
real SoM (`E1M-AEN801`) — it cross-compiles and needs the Zephyr SDK
toolchain, it is not a `native_sim` build (`--native` selects how
`tan` runs the build tooling, not the board target).  The actual
`native_sim` build of this example runs through twister instead, the
same way [`docs/testing.md`](testing.md)'s Zephyr suite does:

```bash
# Linux / macOS / WSL2 (native_sim is Linux/macOS only; on Windows
# run this inside WSL2 — there is no native-Windows native_sim target):
cd ../alp-workspace
export ZEPHYR_BASE="$PWD/zephyr"
python3 "$ZEPHYR_BASE/scripts/twister" \
    -T alp-sdk/examples/peripheral-io/gpio-button-led \
    -s alp_sdk.example.gpio_button_led.e1m_evk \
    -p native_sim/native/64 \
    --inline-logs --no-detailed-test-id
```

Its `testcase.yaml` looks for `[gpio] done` on the console as the
pass condition — if twister reports the scenario PASSED, the SDK is
fully functional on your host.

### 6.4 Run the test suite

```bash
# Linux / macOS / WSL:
python3 -m pytest tests/scripts/ -q
```

```powershell
# Windows native (PowerShell):
python -m pytest tests\scripts\ -q
```

Expected: ~370+ passing tests; matches the Linux baseline.  Any
divergence here is a cross-platform regression and should be
filed as an issue.

---

## 7. Known gotchas

### 7.1 Windows path-length limit (`MAX_PATH = 260`)

Build dirs can blow past Windows's default 260-character path
limit.  Two mitigations:

```powershell
# Option A: enable long-path support (Windows 10 1607+).
# Run as Administrator:
New-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem" `
    -Name "LongPathsEnabled" -Value 1 -PropertyType DWORD -Force
# Then enable in Git too:
git config --system core.longpaths true
```

Option B (no admin rights needed): keep the workspace shallow.
Avoid `C:\Users\<long-name>\Documents\GitHub\alp-workspace\...`
— use `C:\dev\alp-workspace\` or `D:\alp-workspace\`.

### 7.2 Antivirus interference on Windows

Windows Defender real-time scanning slows down Zephyr build dirs
significantly (object files trigger heuristic scans).  Exclude
the build directory:

```powershell
Add-MpPreference -ExclusionPath "$HOME\dev\alp-workspace"
```

(Requires admin PowerShell.)  Corporate AV products may have
similar exclude lists in their management consoles.

### 7.3 CRLF line endings on Windows

The repo's `.gitattributes` pins LF on every source file.  Run

```powershell
git config --global core.autocrlf input
```

once after install — this tells git "I don't want CRLF conversion
on checkout".  Without it, `clang-format-diff` CI will trip on
your contributions.

### 7.4 macOS Gatekeeper on the Arm GNU Toolchain

The first time you run `arm-none-eabi-gcc` from the arm.com
tarball, Gatekeeper flags the binary.  Either:

```bash
# Per-file, on first invocation:
xattr -dr com.apple.quarantine /opt/arm-gnu
```

or open *System Settings → Privacy & Security* and click "Allow
Anyway" on the prompt.  The Homebrew cask install does not have
this issue (homebrew signs / notarises its bottle).

### 7.5 Python `python` vs `python3`

On Linux and macOS, `python` typically points at Python 2 (or
nothing).  Always invoke as `python3`.

On Windows, `python` and `python3` both work after a python.org
install; `py` is the Python Launcher and `py -3.12` selects a
specific version.

On Windows + WSL, follow the Linux convention.

The scripts under `scripts/` use `python3` in their shebang
lines.  On Windows you invoke them as `python scripts\foo.py`
explicitly — Windows does not honour Unix shebangs.

### 7.6 Bash-only scripts

The following scripts under `scripts/` are intentionally Bash:

- `scripts/bootstrap.sh` — fresh-clone setup.  Works on
  Linux / macOS / WSL.  Windows-native users run the PowerShell
  twin `scripts/bootstrap.ps1` (see §4), or follow §4 manually.  Both
  twins read their facts from
  [`metadata/bootstrap.json`](../metadata/bootstrap.json) (issue
  #917) rather than hardcoding them separately.
- `scripts/test-all.sh` — local CI driver.  Works on
  Linux / macOS / WSL.  Windows-native users invoke the
  individual Python tests directly (see §6.4).
- `scripts/setup-clang-format.sh` — pulls a pinned
  `clang-format` binary.  Works on Linux / macOS.

These are not the **only** developer entry points — every
underlying tool (`west`, `cmake`, `python -m pytest`) is
cross-platform — the Bash scripts are convenience wrappers,
not load-bearing for shipping firmware.

### 7.7 Serial device naming

The same USB-serial dongle shows up as:

| Host | Device path |
|---|---|
| Linux | `/dev/ttyUSB0`, `/dev/ttyACM0` |
| macOS | `/dev/cu.usbserial-*`, `/dev/cu.usbmodem*` |
| Windows native | `COM3`, `COM4`, … |
| WSL2 | `/dev/ttyS3` (after usbipd-win passthrough; the number tracks the Windows COM port number) |

Docs that prescribe a single path are wrong by construction —
prefer placeholders like `<your-serial-device>` and let the
reader fill in their OS's convention.  The
`scripts/check_cross_platform.py` lint catches hardcoded
`/dev/...` paths in docs.

### 7.8 Symlinks in git on Windows

Git on Windows does not enable symlink support by default
(requires Developer Mode or admin rights at clone time).  A few
files in the SDK are symlinks for backward-compatibility
filename aliases.  Either:

```powershell
# As admin, enable Developer Mode (Settings → System → For
# developers), then:
git config --global core.symlinks true
git clone https://github.com/alplabai/alp-sdk
```

or accept that the symlinks land as plain text files containing
the link target.  The SDK does not rely on these symlinks for
build correctness; they're documentation aliases only.

---

## 8. What is Linux-only and why

Per [ADR 0012](adr/0012-cross-platform-developer-host.md), the
following workflows are Linux-only — by upstream constraint, not
by SDK choice — and the SDK does not pretend otherwise.

### 8.1 Yocto / OE-core host build

`bitbake`, the metadata parser, and OE-core's build pipeline
all require a Linux userland.  The constraints:

- Case-sensitive filesystem (macOS HFS+/APFS default to
  case-*insensitive*; bitbake parses `meta-foo/recipes-foo/foo.bb`
  vs `meta-foo/recipes-foo/Foo.bb` as distinct files).
- POSIX file modes + extended attributes.
- A working `mmap`-based sstate cache (Windows lacks the relevant
  semantics; even WSL1 fails here, hence WSL2 is required).

There is no plan upstream to port bitbake to Windows or macOS.
Mac / Windows users targeting Yocto use WSL2 (Windows) or a
Linux VM (Mac).

### 8.2 Renesas DRP-AI compiler toolchain (`drp-ai-tvm`)

The DRP-AI model compiler runs on Linux only.  Mac / Windows
users compile DRP-AI models on a Linux machine (CI runner, a
VM, or the EVK's A55 cluster running Yocto).  See
[`docs/vendor-partnerships.md`](vendor-partnerships.md) §Renesas
for the current state.

### 8.3 DEEPX `dx_rt` Linux PCIe driver

The DEEPX DX-M1 runtime is a Linux kernel driver + userspace
library.  It runs on the Linux A55 cluster of the V2N-M1 SoM,
not on the host.  Cross-platform host development against the
**host-side** DEEPX shim (`chips/deepx_dxm1/`) is fine; running
inference against an attached M1 happens on the Linux target.

### 8.4 `meta-alp-sdk` Yocto layer

The SDK's own Yocto layer (`meta-alp-sdk/`) is consumed by
bitbake, so it is **only** exercised on Linux.  The layer itself
is editable from any host (it's plain text + Python recipes), but
verification (`bitbake-layers add-layer; bitbake alp-image`)
runs in Linux only.

### 8.5 The two-line summary

> Anything that ships an artefact for an A-class Linux core
> (Yocto images, DRP-AI compiled models, DEEPX runtime binaries,
> the meta-alp-sdk layer) requires Linux on the host.  Anything
> that ships an artefact for an M-class core (Zephyr firmware,
> bare-metal apps, MCUboot images, gd32-bridge firmware, CC3501E
> firmware) is cross-platform on the host.

If you're targeting an M-class core only, you never need to
leave macOS or Windows.  If you're targeting an A-class core,
you need a Linux environment somewhere in your build pipeline —
locally (Linux host or VM / WSL2) or remotely (CI runner,
shared dev box).
