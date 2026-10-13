# SPDX-License-Identifier: Apache-2.0
"""Dev-kit opkg feed wiring (#2841): the public key, the signing fragment and the
on-target config template must agree, and the packaging script must produce a
feed whose Packages.sig verifies."""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
KEY = REPO / "keys" / "alp_devkit_feed_signing.pub.asc"
LAYER = REPO / "meta-alp-sdk"
SIGN_INC = LAYER / "conf/distro/include/alp-devkit-feed-sign.inc"
RECIPE = LAYER / "recipes-core/alp-devkit-feed/alp-devkit-feed-config_0.1.bb"
TEMPLATE = LAYER / "recipes-core/alp-devkit-feed/files/alp-devkit-feed.conf.in"
PACKAGER = REPO / "scripts/package_devkit_feed.sh"

# gpg-agent sockets under the long Windows/macOS tmp paths fail; the packager is Linux-only.
HAVE_GPG = shutil.which("gpg") and shutil.which("bash") and sys.platform.startswith("linux")


def _fingerprint() -> str:
    out = subprocess.run(
        ["gpg", "--show-keys", "--with-colons", str(KEY)],
        capture_output=True, text=True, encoding="utf-8", check=True).stdout
    return re.search(r"^fpr:+([0-9A-F]{40}):", out, re.M).group(1)


def test_public_key_is_armored_and_public_only():
    text = KEY.read_text(encoding="utf-8")
    assert text.startswith("-----BEGIN PGP PUBLIC KEY BLOCK-----")
    assert "PRIVATE" not in text


def test_template_forces_signature_check_and_has_url_placeholder():
    t = TEMPLATE.read_text(encoding="utf-8")
    assert "option check_signature 1" in t
    assert re.search(r"^src/gz \S+ @ALP_DEVKIT_FEED_URL@$", t, re.M)


def test_recipe_installs_the_committed_key_and_config():
    r = RECIPE.read_text(encoding="utf-8")
    assert KEY.name in r and "alp-devkit-feed.conf" in r and "opkg-key populate" in r
    assert "--import-ownertrust" in r  # opkg rejects a signature from an untrusted key



class _SkipRecipe(Exception):
    pass


class _Fatal(Exception):
    pass


def _run_anonymous_python(feed_url: str | None) -> None:
    """Run the recipe's `python () {}` block the way bitbake does at parse time,
    against a stub datastore holding only ALP_DEVKIT_FEED_URL."""
    body = re.search(r"^python \(\) \{\n(.*?)^\}", RECIPE.read_text(encoding="utf-8"),
                     re.M | re.S).group(1)
    bb = type("bb", (), {
        "parse": type("parse", (), {"SkipRecipe": _SkipRecipe}),
        "fatal": staticmethod(lambda msg: (_ for _ in ()).throw(_Fatal(msg))),
    })
    d = type("d", (), {"getVar": staticmethod(
        lambda name: feed_url if name == "ALP_DEVKIT_FEED_URL" else None)})
    exec("def _anon():\n" + body + "\n_anon()\n", {"bb": bb, "d": d})


def test_recipe_skips_itself_when_the_feed_is_not_enabled():
    # Bitbake parses every recipe BBFILES matches, so a bb.fatal here would break
    # every build that does not opt into the feed (#2853).
    with pytest.raises(_SkipRecipe):
        _run_anonymous_python(None)


def test_recipe_parses_when_the_feed_url_is_set():
    _run_anonymous_python("https://example.invalid/feed")

@pytest.mark.skipif(not shutil.which("gpg"), reason="needs gpg")
def test_signing_fragment_names_the_committed_key():
    assert _fingerprint() in SIGN_INC.read_text(encoding="utf-8")


@pytest.mark.skipif(not HAVE_GPG, reason="needs gpg + bash")
def test_packager_builds_a_verifiable_flat_feed(tmp_path):
    home = tmp_path / "gnupg"
    home.mkdir(mode=0o700)
    env = {"GNUPGHOME": str(home), "PATH": __import__("os").environ["PATH"]}
    subprocess.run(["gpg", "--batch", "--pinentry-mode", "loopback", "--passphrase", "", "--quick-generate-key",
                    "t <t@example.invalid>", "rsa2048", "sign", "never"],
                   env=env, check=True, capture_output=True)
    fpr = re.search(r"^fpr:+(\w{40}):", subprocess.run(
        ["gpg", "--with-colons", "--list-keys"], env=env, capture_output=True,
        text=True, encoding="utf-8", check=True).stdout, re.M).group(1)
    ipk = tmp_path / "ipk"
    for arch in ("all", "e1m_v2n101_a55", "cortexa55"):
        (ipk / arch).mkdir(parents=True)
        (ipk / arch / "Packages").write_text(f"Package: p-{arch}\nFilename: p-{arch}.ipk\n\n", encoding="utf-8")
        (ipk / arch / f"p-{arch}.ipk").write_bytes(b"x")
    out = tmp_path / "out"
    subprocess.run(["bash", str(PACKAGER), "--ipk-dir", str(ipk), "--machine",
                    "e1m-v2n101-a55", "--tag", "feed-v0-e1m-v2n101-a55", "--out",
                    str(out), "--gnupghome", str(home), "--key", fpr],
                   env=env, check=True, capture_output=True)
    names = {p.name for p in out.iterdir()}
    assert {"Packages", "Packages.gz", "Packages.sig", "p-all.ipk",
            "p-e1m_v2n101_a55.ipk", "feed-v0-e1m-v2n101-a55.tar.gz"} <= names
    assert "p-cortexa55.ipk" not in names  # tune arch is not part of the dev-kit feed
