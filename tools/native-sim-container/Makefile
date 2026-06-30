# SPDX-License-Identifier: Apache-2.0
#
# Convenience targets for the hardware-free native_sim reproduction
# container.  Wraps `<engine> build` / `run` so a contributor doesn't
# have to remember the bind-mount + :z relabel incantation.
#
#   make build      # build the image (bakes the pinned Zephyr v4.4.0)
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

.PHONY: build test shell run clean

build:
	$(CONTAINER_ENGINE) build -t $(IMAGE) $(HERE)

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
