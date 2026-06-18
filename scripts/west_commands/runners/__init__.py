# Copyright (c) 2026 Alp Lab AB
#
# SPDX-License-Identifier: Apache-2.0

"""alp-sdk west *module runners*.

These are Zephyr binary runners (flashers/debuggers) that alp-sdk
surfaces over the vendor SDK, registered via ``zephyr/module.yml``'s
``runners:`` list so plain ``west flash`` discovers them without any
edit to the pinned Zephyr tree (ADR-0017).

- alif_flash -- flash an AEN801 (Alif Ensemble E8) image to MRAM with the
  Alif Security Toolkit (SETOOLS), i.e. ``west flash`` == the proven
  bench Flow A recipe. See ``scripts/bench/aen/flash-run.sh`` and
  ``docs/aen-bench-bringup.md``.

NOTE: at ``west flash`` time Zephyr imports each ``file:`` listed under
``runners:`` directly by path (run_common.import_from_path); the runner
module itself imports ``runners.core`` from the *active Zephyr*'s
runners package, not from this directory. This file just marks the
directory as a package for tooling and editors.
"""
