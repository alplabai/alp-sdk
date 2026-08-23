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
#
# The awk pass scans for the FIRST `revision:` line ANYWHERE inside the
# `- name: zephyr` project block, stopping at the next `- name:` entry --
# not just the line immediately following the name (a purely positional
# `getline` breaks the moment west.yml's key order changes, a comment is
# inserted before `revision:`, or the block is reshaped in any way; measured
# false-non-empty output from each of those before this fix).
#
# `[ \t]`, not the POSIX `[[:space:]]` class, inside the awk PROGRAM: the
# one-true-awk (nawk) macOS ships through at least Monterey does not support
# `[[:space:]]` -- it parses the bracket expression as a literal character
# set instead of the space/tab class, so the anchored `- name: zephyr` match
# (4-space-indented in west.yml) never fires there and `ZEPHYR_REV` comes up
# empty on every macOS host running the system awk. Same trap family as
# alp-sdk#1056/#654/#658. sed's own `[[:space:]]` below is untouched --
# BSD sed -E supports the POSIX classes fine; only awk's parser is the
# problem.
ZEPHYR_REV := $(shell awk '/^[ \t]*- name: zephyr[ \t]*$$/{infield=1;next} infield && /^[ \t]*- name: /{exit} infield && /revision:/{print;exit}' $(ALP_SDK_DIR)/west.yml | sed -E 's/^[[:space:]]*revision:[[:space:]]*([^[:space:]]+).*/\1/')

.PHONY: build test shell run clean

build:
	@case "$(ZEPHYR_REV)" in \
		v[0-9]*) ;; \
		*) echo "error: could not derive a valid pinned Zephyr revision from $(ALP_SDK_DIR)/west.yml (got '$(ZEPHYR_REV)')" >&2; exit 1 ;; \
	esac
	$(CONTAINER_ENGINE) build -t "$(IMAGE)" -f "$(HERE)Containerfile" --build-arg ZEPHYR_REV="$(ZEPHYR_REV)" "$(HERE)"

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
