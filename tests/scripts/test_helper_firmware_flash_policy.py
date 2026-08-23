# SPDX-License-Identifier: Apache-2.0
"""#1357: the `helper_firmware_entry` flashing model.

`flash_method`, `update_channel` and `flash_policy` are three INDEPENDENT
axes -- how a helper's image is written locally, how it is updated in the
field, and who may perform the local write.  The schema used to encode
`update_channel` XOR `flash_method`, which cannot describe the GD32
bridge: that part ships with a real OTA path over the bridge link
(protocol v0.6 Path A, `firmware/gd32-bridge/src/ota.c`) AND a
recovery-only SWD flash for a bricked board.

These tests pin the schema half.  `tests/scripts/test_orchestrate_manifest.py`
pins the emission half -- the second, independent XOR that used to DROP
`flash_method`/`flash_args` from the manifest for a both-declaring entry.

`flash_policy` is also unconditionally REQUIRED on every `helper_firmware`
entry (not just when both `flash_method` and `update_channel` are present):
an absent-means-`customer` default left the next SoM port that adds a
helper with only `flash_method` an ungated customer flash target by
omission.
"""

import json
from pathlib import Path

import jsonschema
import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
SOM_SCHEMA = REPO / "metadata/schemas/som-preset-v1.schema.json"
MODULES = REPO / "metadata/e1m_modules"

V2N_V2M_SKUS = ["E1M-V2N101", "E1M-V2N102", "E1M-V2M101", "E1M-V2M102"]
AEN_SKUS = [
    "E1M-AEN301", "E1M-AEN401", "E1M-AEN501",
    "E1M-AEN601", "E1M-AEN701", "E1M-AEN801",
]


def _entry_validator() -> jsonschema.Draft202012Validator:
    """A validator bound to `$defs/helper_firmware_entry` alone.

    Validating the whole preset would force every fixture to carry the
    ~20 unrelated required top-level keys, and a failure anywhere in that
    payload would look like a pass/fail of the rule under test.  Binding
    to the subschema keeps each assertion about the rule it names.
    """
    schema = json.loads(SOM_SCHEMA.read_text(encoding="utf-8"))
    sub = dict(schema["$defs"]["helper_firmware_entry"])
    sub["$schema"] = schema["$schema"]
    return jsonschema.Draft202012Validator(sub)


def _errors(entry: dict) -> list[str]:
    return [e.message for e in _entry_validator().iter_errors(entry)]


GD32_BOTH = {
    "name": "gd32_bridge",
    "chip": "gd32g553",
    # NOT `swd_probe`: #1439 removed that backend from every preset, and a
    # fixture asserting the schema still accepts a both-axes entry must not
    # depend on the one method name that went away.
    "flash_method": "openocd",
    "flash_policy": "recovery_only",
    "update_channel": "alp_ota_spi_bridge",
    "flash_args": {
        "interface": "cmsis-dap",
        "target": "gd32g553",
        "jlink_device": "GD32G553MEY7TR",
        "base": "0x08000000",
    },
}


# ---------------------------------------------------------------------
# The XOR is gone: an entry may carry both halves.
# ---------------------------------------------------------------------


def test_entry_may_declare_both_flash_method_and_update_channel():
    """The GD32 bridge's real shape validates.

    Under the old `oneOf` this entry was rejected outright, so the
    recovery path could not be expressed at all.
    """
    assert _errors(GD32_BOTH) == []


def test_both_halves_without_flash_policy_is_rejected():
    """Both halves + no policy is exactly where 'who may flash it' stops
    being inferable -- and, since `flash_policy` is unconditionally
    required, is also rejected the same way every other flash_policy-less
    entry is."""
    entry = {k: v for k, v in GD32_BOTH.items() if k != "flash_policy"}
    errs = _errors(entry)
    assert any("flash_policy" in m for m in errs), errs


def test_flash_method_alone_now_needs_flash_policy():
    """A plain customer flash target -- every preset written before this
    field -- USED to validate with no `flash_policy` at all; that is
    exactly the silent absent-means-`customer` gap #1357 closes.  A
    helper declaring only `flash_method` and no `flash_policy` must be
    rejected, not read as an implicit customer target."""
    entry = {
        "name": "some_helper",
        "chip": "gd32g553",
        "flash_method": "openocd",
        "flash_args": {"cfg": "board/whatever.cfg"},
    }
    errs = _errors(entry)
    assert any("flash_policy" in m for m in errs), errs


def test_flash_method_alone_with_flash_policy_validates():
    """The same shape, made honest with an explicit `flash_policy`."""
    entry = {
        "name": "some_helper",
        "chip": "gd32g553",
        "flash_method": "openocd",
        "flash_args": {"cfg": "board/whatever.cfg"},
        "flash_policy": "customer",
    }
    assert _errors(entry) == []


def test_update_channel_alone_now_needs_flash_policy():
    """The CC3501E's shape (channel, no method) also used to validate with
    no `flash_policy` -- `flash_policy` is now required regardless of
    which of `flash_method`/`update_channel` an entry carries, or
    neither."""
    entry = {
        "name": "cc3501e_otp",
        "chip": "cc3501e",
        "firmware_path": "firmware/cc3501e/prebuilt/cc3501e-v0.2.0.bin",
        "update_channel": "alp_ota_spi_otp",
    }
    errs = _errors(entry)
    assert any("flash_policy" in m for m in errs), errs


def test_update_channel_alone_with_flash_policy_validates():
    entry = {
        "name": "cc3501e_otp",
        "chip": "cc3501e",
        "firmware_path": "firmware/cc3501e/prebuilt/cc3501e-v0.2.0.bin",
        "update_channel": "alp_ota_spi_otp",
        "flash_policy": "recovery_only",
    }
    assert _errors(entry) == []


def test_neither_flash_method_nor_update_channel_still_needs_flash_policy():
    """A helper with no local flash path and no field-update channel at
    all still must say who may reach it -- the requirement is
    unconditional, not gated on either key's presence."""
    entry = {"name": "some_helper", "chip": "gd32g553"}
    errs = _errors(entry)
    assert any("flash_policy" in m for m in errs), errs


# ---------------------------------------------------------------------
# flash_policy vocabulary.
# ---------------------------------------------------------------------


@pytest.mark.parametrize("policy", ["customer", "factory", "recovery_only"])
def test_flash_policy_accepts_the_three_defined_values(policy):
    entry = dict(GD32_BOTH, flash_policy=policy)
    assert _errors(entry) == []


def test_flash_policy_rejects_an_unknown_value():
    """A typo must fail here, not silently reach a tool that would have
    to guess what an unrecognised policy means."""
    entry = dict(GD32_BOTH, flash_policy="recovery-only")
    errs = _errors(entry)
    assert any("recovery-only" in m for m in errs), errs


# ---------------------------------------------------------------------
# update_channel vocabulary.
# ---------------------------------------------------------------------


def test_update_channel_accepts_the_gd32_bridge_channel():
    entry = dict(GD32_BOTH, update_channel="alp_ota_spi_bridge")
    assert _errors(entry) == []


def test_update_channel_rejects_an_undeclared_channel():
    entry = dict(GD32_BOTH, update_channel="alp_ota_carrier_pigeon")
    errs = _errors(entry)
    assert any("alp_ota_carrier_pigeon" in m for m in errs), errs


def test_gd32_channel_is_distinct_from_the_cc3501e_otp_channel():
    """Reusing `alp_ota_spi_otp` would assert OTP programming for a part
    that has none -- the GD32 streams into a slot-A/B application
    bootloader."""
    schema = json.loads(SOM_SCHEMA.read_text(encoding="utf-8"))
    enum = schema["$defs"]["helper_firmware_entry"]["properties"][
        "update_channel"]["enum"]
    assert "alp_ota_spi_otp" in enum
    assert "alp_ota_spi_bridge" in enum
    assert len(set(enum)) == len(enum)


# ---------------------------------------------------------------------
# #1439 removed the swd_probe `target`/`jlink_device` conditional along
# with the backend it existed for.  `flash_args` shapes are now opaque
# for EVERY backend, with no per-backend exception.
# ---------------------------------------------------------------------


def test_flash_args_shapes_are_opaque_for_every_backend():
    """The conditional this replaces required a `swd_probe` entry naming
    `target` to also name `jlink_device`.  It existed solely because
    tan's `swd_probe` prefers the J-Link arm and refuses rather than
    guess a device profile; with that backend gone from tan there is no
    such rule, and a half-named `flash_args` is legal again."""
    entry = {
        "name": "some_helper",
        "chip": "gd32g553",
        "flash_method": "swd_probe",
        "flash_args": {"target": "gd32g553"},
        "flash_policy": "customer",
    }
    assert _errors(entry) == []


def test_flash_args_tbd_is_still_legal():
    """`flash_args: TBD` is the documented placeholder while a flow is
    unfinalised."""
    entry = {
        "name": "some_helper",
        "chip": "gd32g553",
        "flash_method": "openocd",
        "flash_args": "TBD",
        "flash_policy": "customer",
    }
    assert _errors(entry) == []


def test_flash_policy_is_still_required_without_a_flash_method():
    """Non-vacuity for the two tests above: the schema has not simply
    stopped rejecting things.  #1439 removed one conditional, not the
    `flash_policy` requirement the four GD32 entries still satisfy."""
    entry = {
        "name": "some_helper",
        "chip": "gd32g553",
        "update_channel": "alp_ota_spi_bridge",
    }
    errs = _errors(entry)
    assert any("flash_policy" in m for m in errs), errs


# ---------------------------------------------------------------------
# The in-tree presets.
# ---------------------------------------------------------------------


def _helper_firmware(sku: str) -> list[dict]:
    preset = yaml.safe_load((MODULES / f"{sku}.yaml").read_text(encoding="utf-8"))
    return preset["helper_firmware"]


def test_v2n_v2m_gd32_entries_are_identical_across_the_four_skus():
    """V2N101/V2N102/V2M101/V2M102 are ONE PCB, variant-populated -- the
    GD32 block must not drift between them."""
    blocks = {sku: _helper_firmware(sku) for sku in V2N_V2M_SKUS}
    reference = blocks[V2N_V2M_SKUS[0]]
    for sku in V2N_V2M_SKUS[1:]:
        assert blocks[sku] == reference, f"{sku} drifted from {V2N_V2M_SKUS[0]}"


@pytest.mark.parametrize("sku", V2N_V2M_SKUS)
def test_v2n_v2m_gd32_entry_declares_no_local_flash_path(sku):
    """#1439: GD32 programming is separated out of tan's scope entirely
    (tan-cli#732), so the four E1M-X entries declare NO `flash_method`
    and NO `flash_args` -- tan does not flash this part, and no preset
    names a local flash path for it, so the absence is load-bearing,
    not tidiness."""
    entry = next(e for e in _helper_firmware(sku) if e["name"] == "gd32_bridge")
    assert "flash_method" not in entry, sorted(entry)
    assert "flash_args" not in entry, sorted(entry)


@pytest.mark.parametrize("sku", V2N_V2M_SKUS)
def test_v2n_v2m_gd32_entry_keeps_policy_and_update_channel(sku):
    """The other two axes survive #1439 and are the reason it is not a
    blanket delete.

    `flash_policy` is required on every helper entry with or without a
    `flash_method` -- it answers who may reach a local flash path if one
    is ever added, and the six AEN `cc3501e_otp` entries are that same
    shape.  `update_channel` is the FIELD update path: protocol v0.6
    Path A, slot-A/B application bootloader with commit and rollback
    over the bridge link, not SWD, and validated end to end on silicon.
    """
    entry = next(e for e in _helper_firmware(sku) if e["name"] == "gd32_bridge")
    assert entry["flash_policy"] == "recovery_only"
    assert entry["update_channel"] == "alp_ota_spi_bridge"
    # The helper entry itself stays -- it lost keys, not its existence.
    assert entry["chip"] == "gd32g553"


def test_no_som_preset_declares_swd_probe_anywhere():
    """The repo-wide half of #1439. Per-SKU assertions above pass while a
    seventh preset still declares the removed backend."""
    offenders = []
    for path in sorted(MODULES.glob("*.yaml")):
        doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        for entry in doc.get("helper_firmware") or []:
            if entry.get("flash_method") == "swd_probe":
                offenders.append(f"{path.name}:{entry.get('name')}")
    assert offenders == [], offenders


@pytest.mark.parametrize("sku", AEN_SKUS)
def test_aen_cc3501e_entries_declare_recovery_only_policy(sku):
    entry = next(e for e in _helper_firmware(sku) if e["name"] == "cc3501e_otp")
    assert "flash_policy" in entry, (
        f"{sku} cc3501e_otp does not say WHO may flash it; the skip would "
        f"have to assert the OTA channel as the cause: {sorted(entry)}")
    assert entry["flash_policy"] == "recovery_only", (
        "the AEN SoC itself is the customer's normal `tan flash` target -- "
        "only the CC3501E is not routinely customer-flashed, and even then "
        "only to recover a bricked device with Alp Lab-supplied binaries; "
        "`factory` denies that recovery path entirely")
    assert entry["update_channel"] == "alp_ota_spi_otp"
    assert "flash_method" not in entry


def test_system_manifest_schema_declares_every_projectable_helper_key():
    """`helper_mcus[]` is `additionalProperties: true`, so an emitted key
    the schema never mentions validates silently -- which is how
    `update_channel` reached the manifest undeclared.  Consumers read the
    schema as the contract, so every key the SoM preset can put in a
    helper entry must be declared on the manifest side too.
    """
    som = json.loads(SOM_SCHEMA.read_text(encoding="utf-8"))
    manifest = json.loads(
        (REPO / "metadata/schemas/system-manifest-v1.schema.json")
        .read_text(encoding="utf-8"))

    preset_keys = set(
        som["$defs"]["helper_firmware_entry"]["properties"])
    declared = set(
        manifest["properties"]["helper_mcus"]["items"]["properties"])

    missing = sorted(preset_keys - declared)
    assert not missing, (
        f"projectable helper key(s) undeclared in the manifest schema: "
        f"{missing}")


def test_aen_cc3501e_entries_are_identical_across_the_six_skus():
    """metadata/e1m_modules/README.md: the six AEN SKUs must stay in
    lockstep on this block."""
    blocks = {sku: _helper_firmware(sku) for sku in AEN_SKUS}
    reference = blocks[AEN_SKUS[0]]
    for sku in AEN_SKUS[1:]:
        assert blocks[sku] == reference, f"{sku} drifted from {AEN_SKUS[0]}"
