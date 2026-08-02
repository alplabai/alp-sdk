#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Cross-platform scope: sourced only by scripts/test-all.sh (a Linux/WSL/
# macOS tool) and pr-metadata-validate.yml's ubuntu-latest `run:` steps --
# never invoked on native Windows.
#
# Single source for the rpmsg-imx93 board.yaml sweep exclusion (#1025):
# E1M-NX9101's only hw_rev (imx93 r1) is `status: tbd`, refused outright by
# the hw_rev-buildable gate (exit 5) -- not a validation defect, the gate
# correctly refusing an unbuildable SoM. The example's source + README stay
# (see the README's own #1025 note); only the requirement that its
# board.yaml VALIDATE cleanly is lifted.
#
# Sourced by scripts/test-all.sh and .github/workflows/pr-metadata-validate.yml
# so the two board.yaml sweeps read the same pattern instead of each carrying
# its own copy -- they drifted apart exactly that way in alp-sdk#1109
# (test-all.sh was missing this exclusion entirely, so a clean origin/dev
# went red locally on a gate CI passes).
#
# Remove this exclusion once
# metadata/e1m_modules/imx93/hw-revisions.yaml:r1 carries a buildable status.
BOARD_YAML_SWEEP_EXCLUDE_PATTERN='^examples/multicore/rpmsg-imx93/board\.yaml$'
