# SPDX-License-Identifier: Apache-2.0
"""Mutation-direction tests for scripts/sentinels.is_tbd() (issue #1048).

The whole point of the helper is to be stricter than a bare `== "TBD"` in
one direction (case/whitespace) while staying an exact match, not a
substring match, in the other. Each test below is written to fail if
`is_tbd()` regresses toward one of those two wrong shapes.
"""

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
