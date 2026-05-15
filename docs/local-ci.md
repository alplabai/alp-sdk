# Local CI: running twister + smoke builds on your dev box

This is the page for: *"how do I reproduce the GitHub Actions
twister run on my own machine so I'm not waiting on push → CI
roundtrip every iteration?"*

The CI workflow (`.github/workflows/pr-twister.yml`) runs natively
on `ubuntu-latest` (no docker container) with `ZEPHYR_TOOLCHAIN_VARIANT=host`
so the `native_sim/native/64` build uses the runner's stock gcc.
That makes the CI ~5 min and avoids the 17 GB Zephyr CI Docker image.
Doing the same setup on a dev box once gives you a ~30-second
iteration cycle for the same checks.

## What you need

| Piece                          | Why                                                 |
|--------------------------------|-----------------------------------------------------|
| Zephyr v4.4.0 (pinned)         | Twister, board files, the kernel itself.            |
| Zephyr SDK 1.0.1 (arm-eabi)    | Cross-compiler for the `*.aen` scenarios — optional for native_sim. |
| System gcc + g++               | `native_sim/native/64` builds use host gcc (CI does the same). |
| `dtc` (devicetree compiler)    | Preprocesses board `.dts` files.                    |
| `gperf`                        | Kconfig hash tables.                                |
| Python 3.10+                   | `west`, twister, project loader, pytest tests.      |
| `west` (`pip install west`)    | Manifest tool that owns the workspace.              |

The SDK supports two host layouts:

- **WSL2 on Windows** (recommended for `native_sim` builds; Zephyr
  docs explicitly say native_sim is Linux/macOS only).
- **Pure Windows** (works for the cross-compiled `*.aen` scenarios
  only; falls back to MSYS2 for host tools).

Pick whichever matches your day-to-day workflow.  You can have
both side-by-side -- the Windows-side and WSL-side Zephyr
workspaces don't share files.

## Path A — WSL2 (recommended for full CI parity)

This is the path that matches GitHub Actions exactly.

1. **Make sure WSL2 + Ubuntu are installed.**
   `wsl --status` should report at least one Ubuntu distribution.
   If not: `wsl --install -d Ubuntu` from an elevated PowerShell.

2. **Inside WSL Ubuntu, install the build deps** (once):
   ```sh
   sudo apt update
   sudo apt install -y python3-pip python3-venv git cmake ninja-build \
                       gcc g++ device-tree-compiler gperf
   pip3 install --user west
   ```

3. **Clone Zephyr v4.4.0 into your home directory.**  This keeps
   the workspace on WSL's native ext4 (Linux file-system speed),
   not the slower 9P mount of `C:\`:
   ```sh
   west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.4.0 ~/zephyrproject
   cd ~/zephyrproject
   west update
   pip3 install --user -r zephyr/scripts/requirements.txt
   ```

4. **Hook your alp-sdk checkout in via `EXTRA_ZEPHYR_MODULES`.**
   Your repo lives on the Windows side; WSL sees it under
   `/mnt/c/Users/<you>/Documents/GitHub/alp-sdk`.  Add the
   following lines to `~/.bashrc` so every shell has them:
   ```sh
   export ZEPHYR_BASE=$HOME/zephyrproject/zephyr
   export ZEPHYR_TOOLCHAIN_VARIANT=host
   export EXTRA_ZEPHYR_MODULES=/mnt/c/Users/<you>/Documents/GitHub/alp-sdk
   ```
   For `*.aen` cross-compiled scenarios you also need the Zephyr
   SDK; install it under `~/zephyr-sdk-1.0.1/` and add:
   ```sh
   export ZEPHYR_SDK_INSTALL_DIR=$HOME/zephyr-sdk-1.0.1
   ```

5. **Run twister.**  From the alp-sdk root (Windows path is fine):
   ```sh
   wsl -d Ubuntu -- bash -lc '
     cd /mnt/c/Users/<you>/Documents/GitHub/alp-sdk &&
     python3 $ZEPHYR_BASE/scripts/twister \
        --testsuite-root examples \
        -p native_sim/native/64 \
        --build-only'
   ```
   For a single example, add `-s alp_sdk.examples.<name>.native_sim`.

Build artefacts land in `twister-out/` on the Windows side --
your editor sees them immediately.

### What's slow on WSL

WSL2 reads `/mnt/c/...` over a 9P interop bridge.  CMake's
configure step does ~hundreds of `stat()` calls, which adds ~5-10 s
per build over the native-ext4 path.  For very tight iteration
(one example, multiple builds in a row), `git clone` alp-sdk into
WSL's `~/` and use the native-side clone -- compilation drops to
~10 s per build.

## Path B — Pure Windows

This is the path for cross-compiled builds only (Alif Ensemble,
Renesas V2N, NXP i.MX 93 targets via arm-zephyr-eabi).  Native_sim
builds are not supported on Windows by upstream Zephyr.

1. **Install Python 3.10+** from python.org (the Microsoft Store
   variant works but the launcher has worse PATH ergonomics).

2. **`pip install west`** in your user site-packages:
   ```pwsh
   python -m pip install --user --upgrade west
   ```
   Then add the user Scripts dir to your User PATH (one-time):
   ```pwsh
   $scripts = (python -c "import sysconfig; print(sysconfig.get_paths()['scripts'])")
   [Environment]::SetEnvironmentVariable('Path', "$([Environment]::GetEnvironmentVariable('Path','User'));$scripts", 'User')
   ```

3. **Install MSYS2** (https://www.msys2.org) and grab the host
   tools the SDK build invokes:
   ```sh
   pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-dtc mingw-w64-x86_64-gperf
   ```
   Add `C:\msys64\mingw64\bin` to your User PATH.

4. **Initialise the Zephyr workspace** somewhere off `C:\`'s root
   (Windows MAX_PATH bites long build paths):
   ```pwsh
   west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.4.0 C:\Users\<you>\Documents\GitHub\zephyrproject
   cd C:\Users\<you>\Documents\GitHub\zephyrproject
   west update
   python -m pip install --user -r zephyr\scripts\requirements.txt
   ```

5. **Install the Zephyr SDK 1.0.1** (minimal + arm-zephyr-eabi
   only is enough; ~1.5 GB).  Download from
   `github.com/zephyrproject-rtos/sdk-ng/releases/v1.0.1`,
   extract `zephyr-sdk-1.0.1_windows-x86_64_minimal.7z` to
   `C:\Users\<you>\zephyr-sdk-1.0.1\`, then unpack the
   `toolchain_windows-x86_64_arm-zephyr-eabi.7z` into the same
   directory.  Register with CMake:
   ```pwsh
   cd C:\Users\<you>\zephyr-sdk-1.0.1
   cmake -P cmake\zephyr_sdk_export.cmake
   ```

6. **Persist env vars** (User scope -- survives reboots):
   ```pwsh
   [Environment]::SetEnvironmentVariable('ZEPHYR_BASE', 'C:\Users\<you>\Documents\GitHub\zephyrproject\zephyr', 'User')
   [Environment]::SetEnvironmentVariable('ZEPHYR_SDK_INSTALL_DIR', 'C:\Users\<you>\zephyr-sdk-1.0.1', 'User')
   [Environment]::SetEnvironmentVariable('ZEPHYR_TOOLCHAIN_VARIANT', 'zephyr', 'User')
   [Environment]::SetEnvironmentVariable('EXTRA_ZEPHYR_MODULES', 'C:\Users\<you>\Documents\GitHub\alp-sdk', 'User')
   ```

7. **Run a cross-compiled build** to confirm everything's wired:
   ```pwsh
   west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples\drone-autopilot
   ```

`native_sim` builds on Windows will fail at the DTS preprocess
step with `Could not find CMAKE_C_COMPILER using the following
names: gcc` (it expects a POSIX gcc; MSYS2 doesn't satisfy
Zephyr's expected layout for `native_sim`).  Cross to the WSL
path for those.

## Cheap pre-push checks (no toolchain needed)

You don't need any of the above to catch the most common CI
breakers.  Before pushing:

```sh
# YAML syntax across every testcase / metadata file
python -c "import yaml,glob; [yaml.safe_load(open(f)) for f in glob.glob('examples/**/testcase.yaml', recursive=True)]"
python -c "import yaml,glob; [yaml.safe_load(open(f)) for f in glob.glob('metadata/**/*.yaml', recursive=True)]"

# Loader smoke
pytest tests/scripts/ -q
```

These run in seconds and catch 80% of the regressions that
require a CI roundtrip otherwise -- the YAML indentation bugs,
the metadata schema violations, the loader's emitter logic.

## See also

- [`docs/getting-started.md`](getting-started.md) -- one-shot setup
  walkthrough for the **consumer** path (you're using the SDK in
  an application).  This page is the **contributor** equivalent.
- [`docs/zephyr-version-policy.md`](zephyr-version-policy.md) --
  why we pin to v4.4.0.
- [`docs/troubleshooting.md`](troubleshooting.md) -- diagnostic
  cookbook for common build failures.
