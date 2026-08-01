# SPDX-License-Identifier: Apache-2.0
"""Mutation-direction tests for scripts/sentinels.is_tbd() (issue #1048).

The whole point of the helper is to be stricter than a bare `== "TBD"` in
one direction (case/whitespace) while staying an exact match, not a
substring match, in the other. Each test below is written to fail if
`is_tbd()` regresses toward one of those two wrong shapes.

The second half of this file goes one step further: it drives real call
sites that route through `is_tbd()`, not the helper in isolation.
Mutation-testing the helper alone doesn't protect a call site that reverts
to a bare `== "TBD"` -- these fail exactly that regression.
"""

from types import SimpleNamespace

from sentinels import is_tbd


def test_is_tbd_normalises_case_and_whitespace():
    """Fails if the helper regresses to a bare `value == "TBD"` comparison."""
    assert is_tbd("TBD")
    assert is_tbd("tbd")
    assert is_tbd("Tbd")
    assert is_tbd(" TBD ")
    assert is_tbd("\tTBD\n")


def test_is_tbd_rejects_substring_and_non_placeholder_values():
    """Fails if the helper becomes an over-broad substring match."""
    assert not is_tbd("TBD_RESERVED")
    assert not is_tbd("NOT_TBD")
    assert not is_tbd("TBDX")
    assert not is_tbd("real_value")
    assert not is_tbd("")
    assert not is_tbd(None)
    assert not is_tbd(0x2000_0000)  # a real integer base address, not a string


# ---------------------------------------------------------------------
# Call-site regressions -- a hand-typed lowercase "tbd" is schema-valid
# (som-preset-v1 $defs.chip_ref accepts it as a chip-id slug), so a site
# that reverts to `== "TBD"` doesn't just miss a style nit: it emits
# CONFIG_ALP_SDK_CHIP_TBD / a bogus wireless provider for a chip that
# doesn't exist.
# ---------------------------------------------------------------------


def test_slugs_from_on_module_treats_lowercase_tbd_as_sentinel():
    """Reverting alp_orchestrate.slugs._slugs_from_on_module's `is_tbd(val)`
    to `val == "TBD"` would let a hand-typed `wifi_ble: tbd` (the natural
    lowercase spelling every real value in that field uses) through as a
    chip slug."""
    from alp_orchestrate.slugs import _slugs_from_on_module

    assert _slugs_from_on_module({"wifi_ble": "tbd"}) == []
    assert _slugs_from_on_module({"wifi_ble": "cc3501e"}) == ["cc3501e"]


def test_slugs_from_helper_firmware_treats_lowercase_tbd_as_sentinel():
    """Same regression on alp_orchestrate.slugs._slugs_from_helper_firmware's
    `chip:` field."""
    from alp_orchestrate.slugs import _slugs_from_helper_firmware

    assert _slugs_from_helper_firmware([{"chip": "tbd"}]) == []
    assert _slugs_from_helper_firmware([{"chip": "cc3501e"}]) == ["cc3501e"]


def test_wireless_provider_treats_lowercase_tbd_as_sentinel():
    """Reverting alp_orchestrate.kconfig._wireless_provider's `is_tbd(provider)`
    to `provider == "TBD"` would generate provider-specific Kconfig for a
    lowercase `wifi_ble: tbd` placeholder."""
    from alp_orchestrate.kconfig import _wireless_provider

    project = SimpleNamespace(som_preset={"on_module": {"wifi_ble": "tbd"}})
    assert _wireless_provider(project) is None

    project = SimpleNamespace(
        som_preset={"on_module": {"wifi_ble": "cc3501e"}})
    assert _wireless_provider(project) == "cc3501e"


def test_check_pad_first_treats_lowercase_tbd_as_sentinel(tmp_path, monkeypatch):
    """Reverting check_pin_conflicts._check_pad_first's `is_tbd(pad)` to
    `pad == "TBD"` would let two rows sharing a lowercase `tbd` pad read as
    a real dual-claim conflict instead of two un-mapped placeholders."""
    import check_pin_conflicts

    monkeypatch.setattr(check_pin_conflicts, "MODULES", tmp_path)
    rel = "family/two-tbd.tsv"
    tsv_path = tmp_path / rel
    tsv_path.parent.mkdir(parents=True)
    tsv_path.write_text(
        "peripheral\tpad\nUART0\ttbd\nUART1\ttbd\n", encoding="utf-8")

    assert check_pin_conflicts._check_pad_first(rel) == []
