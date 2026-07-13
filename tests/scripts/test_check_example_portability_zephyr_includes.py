# SPDX-License-Identifier: Apache-2.0
"""Direct-zephyr-driver-include lint tests for
scripts/check_example_portability.py (issue #520)."""

from __future__ import annotations

import sys
import textwrap
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_example_portability as portability  # noqa: E402


def _write(root: Path, rel: str, body: str) -> Path:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path


def test_direct_zephyr_display_include_is_rejected(tmp_path: Path) -> None:
    _write(
        tmp_path,
        "src/main.c",
        """
        #include <zephyr/drivers/display.h>

        int main(void) { return 0; }
        """,
    )
    errors = portability.check_no_zephyr_driver_includes(
        tmp_path, "display/some-new-example")
    assert len(errors) == 1
    assert "zephyr/drivers/display.h" in errors[0]
    assert "alp_gui_lvgl_attach" in errors[0]


def test_portable_alp_display_include_passes(tmp_path: Path) -> None:
    _write(
        tmp_path,
        "src/main.c",
        """
        #include "alp/display.h"
        #include "alp/gui.h"

        int main(void) { return 0; }
        """,
    )
    assert portability.check_no_zephyr_driver_includes(
        tmp_path, "display/some-new-example") == []


def test_commented_out_include_does_not_count(tmp_path: Path) -> None:
    _write(
        tmp_path,
        "src/main.c",
        """
        /* #include <zephyr/drivers/display.h> -- migrated off this */

        int main(void) { return 0; }
        """,
    )
    assert portability.check_no_zephyr_driver_includes(
        tmp_path, "display/some-new-example") == []


def test_allowlisted_example_key_is_skipped_entirely(tmp_path: Path) -> None:
    _write(
        tmp_path,
        "src/main.c",
        """
        #include <zephyr/drivers/mdio.h>

        int main(void) { return 0; }
        """,
    )
    # Every real allowlist entry must carry a non-empty reason string.
    for key, reason in portability._ZEPHYR_DRIVER_INCLUDE_ALLOWLIST.items():
        assert isinstance(key, str) and key
        assert isinstance(reason, str) and reason

    assert "v2n/v2n-ethernet-dual" in portability._ZEPHYR_DRIVER_INCLUDE_ALLOWLIST
    assert portability.check_no_zephyr_driver_includes(
        tmp_path, "v2n/v2n-ethernet-dual") == []


def test_non_allowlisted_example_with_multiple_drivers_reports_each(
    tmp_path: Path,
) -> None:
    _write(
        tmp_path,
        "src/main.c",
        """
        #include <zephyr/drivers/gpio.h>
        #include <zephyr/drivers/pwm.h>

        int main(void) { return 0; }
        """,
    )
    errors = portability.check_no_zephyr_driver_includes(
        tmp_path, "some-family/some-new-example")
    assert len(errors) == 2
    assert any("gpio.h" in e for e in errors)
    assert any("pwm.h" in e for e in errors)
