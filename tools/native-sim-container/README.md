# Hardware-free `native_sim` build container

A small, reproducible container that **freezes the `pr-twister.yml` recipe** so
you can run the exact PR gate locally — `podman build … && podman run …` builds
the ztest + example suite under `native_sim/native/64` the same way CI does, with
no hardware and no hand-rolled west/Zephyr setup.

It is **dev/CI tooling only** — nothing here ships in a build or changes runtime
behaviour.

## What it pins

Everything tracks the PR gate (`.github/workflows/pr-twister.yml`) and the SDK's
`west.yml` in lockstep:

| Thing | Pin | Source of truth |
| --- | --- | --- |
| Zephyr | derived from `west.yml` at build time | `west.yml` `zephyr` revision / `pr-twister.yml --mr` |
| Ubuntu base | `24.04` | `pr-twister.yml` runs on `ubuntu-latest` |
| Python | `3.12` | `.python-version` (via `pr-twister.yml` `setup-python`); image itself uses `ubuntu:24.04` system python |
| Toolchain | host `gcc` (`ZEPHYR_TOOLCHAIN_VARIANT=host`) | `pr-twister.yml` `env:` |

There is **no Zephyr SDK** in the image: `native_sim/native/64` compiles with the
host `gcc`, so the ~17 GB cross-toolchain bundle isn't needed — the same reasoning
`pr-twister.yml` documents for skipping the `zephyrprojectrtos/ci` image. The
pinned Zephyr workspace is baked into the image at build time (`west init` /
`update --narrow --depth=1`), so each `run` is a pure compile with no network.

`make build`/`make test`/`make shell` read the Zephyr revision straight out of
`west.yml` and pass it as `--build-arg ZEPHYR_REV=...`, so the image always
bakes whatever this checkout's `west.yml` pins — there is no copy to keep in
sync by hand. The Containerfile's own `ARG ZEPHYR_REV` default exists only
for a standalone `docker build`/`podman build` that bypasses the Makefile;
`scripts/check_bootstrap_manifest.py` fails the PR if that default ever
drifts from `west.yml` again (issue #1458).

## Quick start

The Makefile is the supported entry point: it derives the pinned Zephyr
revision from `west.yml` and passes it as `--build-arg`, adds the `-f
Containerfile` flag `docker` needs (podman auto-detects `Containerfile`; `-f`
is harmless there too), and handles the bind-mount + SELinux `:z` relabel.
From the repo root:

```sh
make -C tools/native-sim-container test      # build (if needed) + run the suite
make -C tools/native-sim-container shell     # interactive shell in the workspace
make -C tools/native-sim-container clean     # remove the image
```

`docker` works too:

```sh
make -C tools/native-sim-container CONTAINER_ENGINE=docker test
```

### Raw engine commands

Bypassing the Makefile works, but then `-f` is on you (`docker` needs it;
`podman` doesn't), and the Zephyr pin comes from the Containerfile's own
`ARG ZEPHYR_REV` default instead of a live `west.yml` read — see "Bumping the
Zephyr pin" below.

```sh
podman build -t alp-native-sim -f tools/native-sim-container/Containerfile tools/native-sim-container
docker build -t alp-native-sim -f tools/native-sim-container/Containerfile tools/native-sim-container

# run the full pr-twister suite against this checkout
podman run --rm -v "$PWD":/work/alp-sdk:z alp-native-sim
```

## Running a narrower build

Any argv after the image name is exec'd verbatim inside the baked workspace
(`ZEPHYR_BASE` and `EXTRA_ZEPHYR_MODULES` are already exported), so you can scope
twister to a single example instead of the whole suite:

```sh
podman run --rm -v "$PWD":/work/alp-sdk:z alp-native-sim \
    python3 zephyr/scripts/twister \
      --testsuite-root /work/alp-sdk/examples/peripheral-io/hello-world \
      -p native_sim/native/64 --inline-logs --no-detailed-test-id
```

## Bumping the Zephyr pin

`west.yml` is **not** where you edit this — it is itself one of the sites
[`scripts/check_bootstrap_manifest.py --fix`](../../scripts/check_bootstrap_manifest.py)
rewrites, not a place to hand-edit. `metadata/bootstrap.json`'s
`zephyr.version` is the actual single source of truth for the whole repo
(issue #917); editing `west.yml` alone leaves it disagreeing with that file
and fails the gate. See
[`docs/zephyr-version-policy.md`](../../docs/zephyr-version-policy.md) for
the full bump procedure. The short version, from the repo root:

```sh
# 1. edit metadata/bootstrap.json's zephyr.version
# 2. propagate it to west.yml, the CI workflow --mr/cache-key pins, the
#    README badge, and this Containerfile's ARG ZEPHYR_REV default:
python3 scripts/check_bootstrap_manifest.py --fix
# 3. prove every pin agrees:
python3 scripts/check_bootstrap_manifest.py
```

`make build`/`make test`/`make shell` then pick up the new pin automatically
on the next run — they read `west.yml` live, so there is nothing to edit in
this directory itself.

Building with a raw `docker`/`podman` command instead of the Makefile? Step
2 above already rewrote the Containerfile's `ARG ZEPHYR_REV` default, so a
standalone build already carries the right value; pass your own
`--build-arg ZEPHYR_REV=<rev>` only if you want to override it.
`scripts/check_bootstrap_manifest.py` (no `--fix`) fails the PR if that
default and `metadata/bootstrap.json` ever disagree, so it can't drift
silently the way it did before issue #1458.

Keep `west.yml`'s `zephyr` revision and `pr-twister.yml`'s `--mr` flag
identical — that lockstep is the whole point of this container.

## Layout

| File | Purpose |
| --- | --- |
| `Containerfile` | the frozen recipe (Ubuntu 24.04 + host gcc + baked Zephyr) |
| `entrypoint.sh` | runs the `pr-twister.yml` twister step, or your own argv |
| `Makefile` | `build` / `test` / `run` / `shell` / `clean` convenience targets |
