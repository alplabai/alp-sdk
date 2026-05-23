from pathlib import Path

from alp_cli.diagnostic import Diagnostic, render


def _sample_source() -> str:
    return (
        "som:\n"
        "  sku: E1M-AEN701\n"
        "preset: e1m-evk\n"
        "peripherals:\n"
        "  - { pad: P21, signal: I2C0_SCL }\n"
    )


def test_render_error_includes_code_path_caret_hint_and_doclink(tmp_path: Path):
    src = _sample_source()
    fixture = tmp_path / "board.yaml"
    fixture.write_text(src)
    diag = Diagnostic(
        severity="error",
        path=fixture,
        line=5,
        col=11,
        span=3,
        code="ALP-B005",
        message="pad 'P21' not present on E1M-AEN701",
        hint="did you mean 'P20'? (closest match, distance 1)",
        doc_url=None,
    )

    out = render(diag, source_text=src, color=False)
    assert "error[ALP-B005]" in out
    assert "board.yaml:5:11" in out
    assert "  - { pad: P21" in out
    assert "^^^" in out
    assert "did you mean 'P20'" in out
    assert "docs/diagnostics/ALP-B005.md" in out


def test_render_omits_color_codes_when_color_false(tmp_path: Path):
    src = "som:\n  sku: bogus\n"
    fixture = tmp_path / "board.yaml"
    fixture.write_text(src)
    diag = Diagnostic(
        severity="error",
        path=fixture,
        line=2,
        col=8,
        span=5,
        code="ALP-B003",
        message="bad sku",
        hint=None,
        doc_url=None,
    )
    out = render(diag, source_text=src, color=False)
    assert "\x1b[" not in out


def test_render_respects_no_color_env(tmp_path: Path, monkeypatch):
    src = "som:\n  sku: x\n"
    fixture = tmp_path / "board.yaml"
    fixture.write_text(src)
    diag = Diagnostic(
        severity="error",
        path=fixture,
        line=2,
        col=8,
        span=1,
        code="ALP-B003",
        message="bad sku",
    )
    monkeypatch.setenv("NO_COLOR", "1")
    out = render(diag, source_text=src, color=None)
    assert "\x1b[" not in out
