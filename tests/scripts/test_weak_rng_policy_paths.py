# SPDX-License-Identifier: Apache-2.0
"""Both halves of the weak-RNG policy, evaluated over the real Kconfig text.

`tests/scripts/test_weak_rng_optin_is_not_shippable.py` polices WHERE the
opt-in may be enabled.  This file polices WHAT the policy decides: it lifts
`ALP_SDK_MBEDTLS_WEAK_RNG`'s `default y if` expression out of
`zephyr/Kconfig.alp-libraries` and the refusal condition out of
`zephyr/CMakeLists.txt`, and evaluates the pair over a truth table.

Both paths matter, and each one has a way to break quietly:

* The REFUSE path.  `MBEDTLS_PSA_CRYPTO_LEGACY_RNG` is Zephyr's default
  whenever no external CSPRNG is offered, and it `select`s
  `TEST_RANDOM_GENERATOR if !ENTROPY_HAS_DRIVER`
  ($ZEPHYR_BASE/modules/mbedtls/Kconfig.tf-psa-crypto).  That is every Alif
  Ensemble target today (#2192).  If this expression is ever narrowed to
  `!CSPRNG_ENABLED` alone, native_sim's fake driver -- which sets
  `ENTROPY_HAS_DRIVER` while its own help text says it "does not generate
  real entropy" -- reads as a pass, and so would any future fake that
  copies it.
* The ALLOW path.  A target that binds a real entropy driver, and a TF-M
  build where the crypto partition owns the RNG, must build with no opt-in
  and no friction.  A policy that refuses those would be routed around
  rather than fixed.
"""

from __future__ import annotations

import re
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[2]
_KCONFIG = _REPO_ROOT / "zephyr" / "Kconfig.alp-libraries"
_CMAKELISTS = _REPO_ROOT / "zephyr" / "CMakeLists.txt"


def _config_block(text: str, symbol: str) -> str:
    match = re.search(rf"^config {re.escape(symbol)}$", text, re.MULTILINE)
    assert match, f"{_KCONFIG.name} no longer declares `config {symbol}`"
    rest = text[match.end():]
    end = re.search(r"^(config |menuconfig |choice\b|endmenu\b)", rest, re.MULTILINE)
    return rest[: end.start()] if end else rest


def _default_y_expr(symbol: str) -> str:
    block = _config_block(_KCONFIG.read_text(encoding="utf-8"), symbol)
    joined = block.replace("\\\n", " ")
    match = re.search(r"^\s*default y if (.+)$", joined, re.MULTILINE)
    assert match, f"`config {symbol}` no longer carries a `default y if` condition"
    return match.group(1).strip()


def _depends_on_expr(symbol: str) -> str:
    block = _config_block(_KCONFIG.read_text(encoding="utf-8"), symbol)
    joined = block.replace("\\\n", " ")
    match = re.search(r"^\s*depends on (.+)$", joined, re.MULTILINE)
    return match.group(1).strip() if match else "y"


def _cmake_refusal_expr() -> str:
    text = _CMAKELISTS.read_text(encoding="utf-8")
    match = re.search(
        r"^\s*if\s*\((\s*CONFIG_ALP_SDK_MBEDTLS_WEAK_RNG\b[^)]*)\)", text, re.MULTILINE
    )
    assert match, (
        "zephyr/CMakeLists.txt no longer opens an if() on "
        "CONFIG_ALP_SDK_MBEDTLS_WEAK_RNG -- nothing enforces the policy"
    )
    return match.group(1).strip()


def _evaluate(expr: str, symbols: dict[str, bool]) -> bool:
    """Evaluate a Kconfig/CMake boolean expression against a symbol table.

    Only the operators these two expressions actually use are translated;
    anything richer should fail loudly rather than be guessed at.
    """
    python = " ".join(expr.split())
    python = re.sub(r"\bAND\b", "&&", python)
    python = re.sub(r"\bNOT\b", "!", python)
    python = python.replace("&&", " and ").replace("||", " or ")
    python = re.sub(r"!\s*", " not ", python)
    python = re.sub(r"\bCONFIG_", "", python)

    names: dict[str, bool] = {}

    def _name(match: re.Match[str]) -> str:
        token = match.group(0)
        if token in ("and", "or", "not", "y", "n"):
            return {"y": "True", "n": "False"}.get(token, token)
        names[token] = symbols.get(token, False)
        return token

    python = re.sub(r"[A-Za-z_][A-Za-z0-9_]*", _name, python)
    assert not re.search(r"[^A-Za-z0-9_() ]", python), (
        f"unsupported operator in {expr!r} -- this evaluator understands only "
        f"&&, ||, ! and parentheses, so it must be extended rather than "
        f"silently misread"
    )
    return bool(eval(python, {"__builtins__": {}}, names))  # noqa: S307


def _refused(**symbols: bool) -> bool:
    """Would CMake refuse this configuration?"""
    table = dict(symbols)
    table["ALP_SDK_MBEDTLS_PSA_CRYPTO"] = _evaluate(
        _depends_on_expr("ALP_SDK_MBEDTLS_PSA_CRYPTO"), table
    ) and _evaluate(_default_y_expr("ALP_SDK_MBEDTLS_PSA_CRYPTO"), table)
    table["ALP_SDK_MBEDTLS_WEAK_RNG"] = _evaluate(
        _default_y_expr("ALP_SDK_MBEDTLS_WEAK_RNG"), table
    )
    return _evaluate(_cmake_refusal_expr(), table)


#: (name, symbols, refused?) -- the four configurations this PR actually
#: builds, plus the two that must stay frictionless.
_CASES = [
    (
        "AEN today: PSA core on, no Ensemble entropy driver, so Zephyr's "
        "legacy RNG path selects TEST_RANDOM_GENERATOR (#2192)",
        dict(MBEDTLS=True, MBEDTLS_BUILTIN=True, TEST_RANDOM_GENERATOR=True),
        True,
    ),
    (
        "the same AEN build with the acknowledgement on its scenario",
        dict(
            MBEDTLS=True, MBEDTLS_BUILTIN=True, TEST_RANDOM_GENERATOR=True,
            ALP_SDK_ALLOW_TEST_ENTROPY=True,
        ),
        False,
    ),
    (
        "native_sim: CSPRNG_ENABLED is y, but the driver behind it is "
        "FAKE_ENTROPY_NATIVE_SIM, which says it is not real entropy",
        dict(
            MBEDTLS=True, MBEDTLS_BUILTIN=True,
            CSPRNG_ENABLED=True, FAKE_ENTROPY_NATIVE_SIM=True,
        ),
        True,
    ),
    (
        "the same native_sim build with the acknowledgement in native_sim.conf",
        dict(
            MBEDTLS=True, MBEDTLS_BUILTIN=True,
            CSPRNG_ENABLED=True, FAKE_ENTROPY_NATIVE_SIM=True,
            ALP_SDK_ALLOW_TEST_ENTROPY=True,
        ),
        False,
    ),
    (
        "a target that binds a real entropy driver: no opt-in, no friction",
        dict(MBEDTLS=True, MBEDTLS_BUILTIN=True, CSPRNG_ENABLED=True),
        False,
    ),
    (
        "a TF-M build: the secure partition owns crypto, so this SDK never "
        "turns the PSA core on and has no RNG opinion to enforce",
        dict(MBEDTLS=True, MBEDTLS_BUILTIN=True, BUILD_WITH_TFM=True),
        False,
    ),
]


def test_refuse_and_allow_paths_both_decide_correctly() -> None:
    for name, symbols, expected in _CASES:
        actual = _refused(**symbols)
        assert actual == expected, (
            f"{name}: the policy {'refuses' if actual else 'allows'} this "
            f"build but should {'refuse' if expected else 'allow'} it. "
            f"Symbols: {sorted(k for k, v in symbols.items() if v)}."
        )


def test_a_custom_mbedtls_implementation_does_not_inherit_the_policy() -> None:
    """`MBEDTLS_BUILTIN` scoping, asserted as behaviour rather than as text.

    The include-order break this PR fixes is a property of the mbedtls
    sources Zephyr vendors.  The other leg of Zephyr's mbedtls choice is a
    custom implementation, which brings its own headers and its own RNG
    story; this SDK must not reach into it.
    """
    assert not _refused(MBEDTLS=True, TEST_RANDOM_GENERATOR=True), (
        "a build using a custom mbedtls implementation (MBEDTLS_BUILTIN=n) is "
        "being refused by our policy. ALP_SDK_MBEDTLS_PSA_CRYPTO must stay "
        "scoped to MBEDTLS_BUILTIN so we only govern the sources we broke."
    )


def test_the_fake_entropy_term_is_load_bearing() -> None:
    """Guard the guard: prove the native_sim case needs its own term.

    `CSPRNG_ENABLED` alone would pass native_sim, because
    `FAKE_ENTROPY_NATIVE_SIM` selects `ENTROPY_HAS_DRIVER`. If this ever
    stops failing, the expression has been narrowed and the fake reads as a
    real entropy source.
    """
    expr = _default_y_expr("ALP_SDK_MBEDTLS_WEAK_RNG")
    narrowed = re.sub(r"\|\|\s*FAKE_ENTROPY_NATIVE_SIM", "", expr)
    assert narrowed != expr, (
        "FAKE_ENTROPY_NATIVE_SIM is no longer a term of "
        "ALP_SDK_MBEDTLS_WEAK_RNG's condition"
    )
    symbols = dict(
        MBEDTLS=True, MBEDTLS_BUILTIN=True,
        CSPRNG_ENABLED=True, FAKE_ENTROPY_NATIVE_SIM=True,
        ALP_SDK_MBEDTLS_PSA_CRYPTO=True,
    )
    assert _evaluate(expr, symbols) and not _evaluate(narrowed, symbols), (
        "dropping the FAKE_ENTROPY_NATIVE_SIM term must change the verdict on "
        "native_sim -- if it does not, the truth table above is not actually "
        "exercising that term"
    )
