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
| Zephyr | `v4.4.0` | `west.yml` `zephyr` revision / `pr-twister.yml --mr` |
| Ubuntu base | `24.04` | `pr-twister.yml` runs on `ubuntu-latest` |
| Python | `3.12` | `.python-version` (via `pr-twister.yml` `setup-python`); image itself uses `ubuntu:24.04` system python |
| Toolchain | host `gcc` (`ZEPHYR_TOOLCHAIN_VARIANT=host`) | `pr-twister.yml` `env:` |

There is **no Zephyr SDK** in the image: `native_sim/native/64` compiles with the
host `gcc`, so the ~17 GB cross-toolchain bundle isn't needed — the same reasoning
`pr-twister.yml` documents for skipping the `zephyrprojectrtos/ci` image. The
pinned Zephyr workspace is baked into the image at build time (`west init` /
`update --narrow --depth=1`), so each `run` is a pure compile with no network.

## Quick start

From the repo root:

```sh
# build the image (bakes the pinned Zephyr v4.4.0 workspace)
podman build -t alp-native-sim tools/native-sim-container

# run the full pr-twister suite against this checkout
podman run --rm -v "$PWD":/work/alp-sdk:z alp-native-sim
```

Or use the Makefile, which handles the bind-mount + SELinux `:z` relabel for you:

```sh
make -C tools/native-sim-container test      # build (if needed) + run the suite
make -C tools/native-sim-container shell     # interactive shell in the workspace
make -C tools/native-sim-container clean     # remove the image
```

`docker` works too — either pass `make CONTAINER_ENGINE=docker test` or swap
`podman` for `docker` in the raw commands above.

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

When `west.yml` bumps the Zephyr revision, rebuild with the matching tag so the
container and the gate stay in step:

```sh
podman build --build-arg ZEPHYR_REV=v4.5.0 -t alp-native-sim tools/native-sim-container
```

Keep `ZEPHYR_REV`, `west.yml`'s `zephyr` revision, and `pr-twister.yml`'s `--mr`
flag identical — that lockstep is the whole point of this container.

## Layout

| File | Purpose |
| --- | --- |
| `Containerfile` | the frozen recipe (Ubuntu 24.04 + host gcc + baked Zephyr) |
| `entrypoint.sh` | runs the `pr-twister.yml` twister step, or your own argv |
| `Makefile` | `build` / `test` / `run` / `shell` / `clean` convenience targets |
