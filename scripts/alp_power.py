#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Probe-based power measurement CLI; see scripts/alp_power/ and docs/measuring-inference-energy.md."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from alp_power.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
