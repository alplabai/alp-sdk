# SPDX-License-Identifier: Apache-2.0
"""examples/aen/aen-trace-runner/tools/genlogo.py: a logo image -> the HUD's ARGB4444 palette header.

Synthetic images only (no partner artwork in this repo): palette index 0 is the clear pixel,
alpha rounds to 4 bits, the colour of a clear pixel is dropped, and a logo with more than 256
distinct pixel values is refused rather than truncated.
"""
import importlib.util
import pathlib

import pytest

np = pytest.importorskip("numpy")
Image = pytest.importorskip("PIL.Image")

_PATH = pathlib.Path(__file__).resolve().parents[2] / "examples/aen/aen-trace-runner/tools/genlogo.py"
_spec = importlib.util.spec_from_file_location("genlogo", _PATH)
genlogo = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(genlogo)

SS = genlogo.SS


def _grid(pixels, w, h):
    """RGBA float array at SS x the size, each pixel (r, g, b, a) in 0..255 filling its SS x SS block."""
    a = np.zeros((h * SS, w * SS, 4))
    for i, p in enumerate(pixels):
        y, x = divmod(i, w)
        a[y * SS:(y + 1) * SS, x * SS:(x + 1) * SS] = np.array(p) / 255.0
    return a


def test_argb4444_alpha_rounds_and_clear_pixels_drop_colour():
    px = genlogo.to_argb4444(
        _grid([(255, 255, 255, 255), (255, 0, 0, 128), (10, 20, 30, 0), (0, 0, 255, 255)], 4, 1), 4, 1
    )
    assert px.shape == (1, 4)
    assert px[0, 0] == 0xFFFF  # opaque white
    assert px[0, 1] >> 12 == 8  # 128/255 * 15 = 7.53 -> 8
    assert px[0, 2] == 0  # alpha 0 keeps no colour: the HUD skips it
    assert px[0, 3] == 0xF00F  # opaque blue


def test_render_palette_index_zero_is_clear(tmp_path, monkeypatch):
    img = tmp_path / "t.png"
    Image.new("RGBA", (4, 2), (255, 255, 255, 255)).save(img)
    out = "\n".join(genlogo.render(str(img), "4x2")[0])
    assert "TR_PARTNER_LOGO_W 4" in out and "TR_PARTNER_LOGO_H 2" in out
    assert "tr_partner_logo_pal[TR_PARTNER_LOGO_NPAL] = {\n\t0x0000, 0xFFFF," in out
    # a half-clear image: the clear pixels index 0, the opaque one the palette's other entry
    clear = _grid([(0, 0, 0, 0), (255, 255, 255, 255)], 2, 1)
    monkeypatch.setattr(genlogo, "load", lambda p, tw, th: (clear, 2, 1))
    body, w, h = genlogo.render("x.png", "2x1")
    text = "\n".join(body)
    assert (w, h) == (2, 1) and "TR_PARTNER_LOGO_NPAL 2" in text and "\t0, 1," in text


def test_render_refuses_more_than_256_distinct_values(monkeypatch):
    n = 300
    pixels = [((i * 7) % 256, (i * 13) % 256, (i * 29) % 256, 255) for i in range(n)]
    big = _grid(pixels, n, 1)
    monkeypatch.setattr(genlogo, "load", lambda p, tw, th: (big, n, 1))
    with pytest.raises(SystemExit) as e:
        genlogo.render("x.png", "300x1")
    assert "256" in str(e.value)
