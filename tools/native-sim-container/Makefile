# SPDX-License-Identifier: Apache-2.0
#
# Convenience targets for the hardware-free native_sim reproduction
# container.  Wraps `<engine> build` / `run` so a contributor doesn't
# have to remember the bind-mount + :z relabel incantation.
#
#   make build      # build the image (bakes the Zephyr revision pinned
#                    # in west.yml -- see ZEPHYR_REV below)
#   make test       # run the pr-twister.yml twister suite on this tree
#   make shell      # drop into an interactive shell in the workspace
#   make clean      # remove the built image
#
# Override the engine (defaults to podman; docker also works):
#   make CONTAINER_ENGINE=docker test
#
# Override the alp-sdk checkout to mount (defaults to this repo root):
#   make ALP_SDK_DIR=/path/to/alp-sdk test

CONTAINER_ENGINE ?= podman
IMAGE            ?= alp-native-sim
HERE             := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
# This Makefile lives at tools/native-sim-container/ -> repo root is two up.
ALP_SDK_DIR      ?= $(abspath $(HERE)../..)

# `:z` SELinux relabel is a no-op on non-SELinux hosts but required on
# Fedora/RHEL podman; harmless under docker.
MOUNT := $(ALP_SDK_DIR):/work/alp-sdk:z

# Derive the pinned Zephyr revision straight from west.yml instead of
# keeping a second, hand-maintained copy in the Containerfile (issue #1458:
# the Containerfile's own `ARG ZEPHYR_REV` default drifted a patch release
# behind west.yml's `zephyr` revision, silently, with nothing to catch it).
# `make build` always passes this LIVE value as --build-arg, so it can never
# go stale the way the old baked-in default did; the ARG's own default in
# the Containerfile is kept only as a fallback for a standalone `docker
# build`/`podman build` invocation that bypasses this Makefile, and
# scripts/check_bootstrap_manifest.py now gates that default against
# metadata/bootstrap.json so it can't drift silently either.
ZEPHYR_REV := $(shell awk '/^[[:space:]]*- name: zephyr[[:space:]]*$$/{getline; print; exit}' $(ALP_SDK_DIR)/west.yml | sed -E 's/^[[:space:]]*revision:[[:space:]]*([^[:space:]]+).*/\1/')

.PHONY: build test shell run clean

build:
	@test -n "$(ZEPHYR_REV)" || { echo "error: could not derive the pinned Zephyr revision from $(ALP_SDK_DIR)/west.yml" >&2; exit 1; }
	$(CONTAINER_ENGINE) build -t $(IMAGE) -f $(HERE)Containerfile --build-arg ZEPHYR_REV=$(ZEPHYR_REV) $(HERE)

test: build
	$(CONTAINER_ENGINE) run --rm -v "$(MOUNT)" $(IMAGE)

# `run` is an alias for `test` that does NOT force a rebuild -- use it
# for a fast re-run against an already-built image.
run:
	$(CONTAINER_ENGINE) run --rm -v "$(MOUNT)" $(IMAGE)

shell: build
	$(CONTAINER_ENGINE) run --rm -it -v "$(MOUNT)" $(IMAGE) bash

clean:
	-$(CONTAINER_ENGINE) rmi $(IMAGE)
