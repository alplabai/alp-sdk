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


def _gpg_env(tmp_path):
    home = tmp_path / "gnupg"
    home.mkdir(mode=0o700)
    env = {"GNUPGHOME": str(home), "PATH": __import__("os").environ["PATH"]}
    subprocess.run(["gpg", "--batch", "--pinentry-mode", "loopback", "--passphrase", "", "--quick-generate-key",
                    "t <t@example.invalid>", "rsa2048", "sign", "never"],
                   env=env, check=True, capture_output=True)
    fpr = re.search(r"^fpr:+(\w{40}):", subprocess.run(
        ["gpg", "--with-colons", "--list-keys"], env=env, capture_output=True,
        text=True, encoding="utf-8", check=True).stdout, re.M).group(1)
    pub = tmp_path / "pub.asc"
    pub.write_bytes(subprocess.run(["gpg", "--armor", "--export", fpr], env=env,
                                   capture_output=True, check=True).stdout)
    return home, env, fpr, pub


def _ipk_tree(tmp_path, extra=()):
    ipk = tmp_path / "ipk"
    for arch in ("all", "e1m_v2n101_a55", "cortexa55"):
        (ipk / arch).mkdir(parents=True)
        pk = [f"alp-sdk-{arch}", f"kernel-module-{arch}", *extra]
        (ipk / arch / "Packages").write_text(
            "".join(f"Package: {n}\nFilename: {n}.ipk\nSHA256sum: {'0' * 64}\n\n" for n in pk), encoding="utf-8")
        for n in pk:
            (ipk / arch / f"{n}.ipk").write_bytes(b"x")
    return ipk


def _pack(tmp_path, ipk, home, env, fpr, extra_args, name="out",
          allow="alp-sdk-all alp-sdk-e1m_v2n101_a55"):
    out = tmp_path / name
    r = subprocess.run(["bash", str(PACKAGER), "--ipk-dir", str(ipk), "--machine",
                        "e1m-v2n101-a55", "--tag", "feed-v0-e1m-v2n101-a55", "--out",
                        str(out), "--gnupghome", str(home), "--key", fpr, "--allow", allow,
                        *extra_args],
                       env=env, capture_output=True, text=True, encoding="utf-8")
    return out, r


@pytest.mark.skipif(not HAVE_GPG, reason="needs gpg + bash")
def test_packager_publishes_only_the_allowlist_and_verifies(tmp_path):
    home, env, fpr, pub = _gpg_env(tmp_path)
    out, r = _pack(tmp_path, _ipk_tree(tmp_path), home, env, fpr, ["--pubkey", str(pub)])
    assert r.returncode == 0, r.stderr
    names = {p.name for p in out.iterdir()}
    assert {"Packages", "Packages.gz", "Packages.sig", "alp-sdk-all.ipk",
            "alp-sdk-e1m_v2n101_a55.ipk", "feed-v0-e1m-v2n101-a55.tar.gz"} <= names
    assert not any(n.startswith(("kernel-module", "alp-sdk-cortexa55")) for n in names)
    assert "kernel-module" not in (out / "Packages").read_text(encoding="utf-8")


@pytest.mark.skipif(not HAVE_GPG, reason="needs gpg + bash")
def test_packager_rejects_a_signature_the_committed_key_does_not_verify(tmp_path):
    home, env, fpr, _ = _gpg_env(tmp_path)  # throwaway key, default (committed) pubkey
    _, r = _pack(tmp_path, _ipk_tree(tmp_path), home, env, fpr, [])
    assert r.returncode != 0


@pytest.mark.skipif(not HAVE_GPG, reason="needs gpg + bash")
def test_packager_fails_on_a_special_character_ipk_name(tmp_path):
    home, env, fpr, pub = _gpg_env(tmp_path)
    _, r = _pack(tmp_path, _ipk_tree(tmp_path, extra=("bad+name",)), home, env, fpr,
                 ["--pubkey", str(pub)], allow="alp-sdk-all alp-sdk-e1m_v2n101_a55 bad+name")
    assert r.returncode != 0 and "outside" in r.stderr


@pytest.mark.skipif(not HAVE_GPG, reason="needs gpg + bash")
def test_packager_refuses_a_non_empty_out_dir(tmp_path):
    home, env, fpr, pub = _gpg_env(tmp_path)
    (tmp_path / "out").mkdir()
    (tmp_path / "out" / "keep").write_text("x", encoding="utf-8")
    _, r = _pack(tmp_path, _ipk_tree(tmp_path), home, env, fpr, ["--pubkey", str(pub)])
    assert r.returncode == 2 and (tmp_path / "out" / "keep").exists()


@pytest.mark.skipif(not HAVE_GPG, reason="needs gpg + bash")
def test_packager_fails_when_an_allowlisted_package_is_missing(tmp_path):
    home, env, fpr, pub = _gpg_env(tmp_path)
    _, r = _pack(tmp_path, _ipk_tree(tmp_path), home, env, fpr, ["--pubkey", str(pub)],
                 allow="alp-sdk-all alp-sdk-e1m_v2n101_a55 not-built")
    assert r.returncode != 0 and "missing from the index" in r.stderr


@pytest.mark.skipif(not HAVE_GPG, reason="needs gpg + bash")
def test_packager_fails_on_a_stanza_without_sha256sum(tmp_path):
    home, env, fpr, pub = _gpg_env(tmp_path)
    ipk = _ipk_tree(tmp_path)
    f = ipk / "all" / "Packages"
    f.write_text(re.sub(r"SHA256sum: .*[\r\n]", "", f.read_text(encoding="utf-8")), encoding="utf-8")
    _, r = _pack(tmp_path, ipk, home, env, fpr, ["--pubkey", str(pub)])
    assert r.returncode != 0 and "SHA256sum" in r.stderr


@pytest.mark.skipif(not HAVE_GPG, reason="needs gpg + bash")
def test_signature_fails_on_a_tampered_packages_file(tmp_path):
    home, env, fpr, pub = _gpg_env(tmp_path)
    out, r = _pack(tmp_path, _ipk_tree(tmp_path), home, env, fpr, ["--pubkey", str(pub)])
    assert r.returncode == 0, r.stderr
    f = out / "Packages"
    f.write_bytes(f.read_bytes().replace(b"Package: alp-sdk-all", b"Package: alp-sdk-al1", 1))
    keyring = tmp_path / "pub.gpg"
    keyring.write_bytes(subprocess.run(["gpg", "--dearmor"], input=pub.read_bytes(), env=env,
                                       capture_output=True, check=True).stdout)
    v = subprocess.run(["gpgv", "--keyring", str(keyring), str(out / "Packages.sig"), str(f)],
                       env=env, capture_output=True)
    assert v.returncode != 0


def test_feed_is_applied_only_in_the_edge_image_with_a_prod_guard():
    imgs = LAYER / "recipes-images"
    common = (imgs / "alp-image-common.inc").read_text(encoding="utf-8")
    assert "ALP_DEVKIT_FEED" not in common
    assert "ALP_DEVKIT_FEED" in (imgs / "alp-image-edge.bb").read_text(encoding="utf-8")
    prod = (imgs / "alp-image-prod.bb").read_text(encoding="utf-8")
    assert "alp_prod_no_devkit_feed" in prod and 'do_rootfs[prefuncs] += "alp_prod_no_devkit_feed"' in prod
    body = re.search(r"^python alp_prod_no_devkit_feed\(\) \{\s*^(.*?)^\}", prod, re.M | re.S).group(1)
    bb = type("bb", (), {"fatal": staticmethod(lambda m: (_ for _ in ()).throw(_Fatal(m)))})

    def run(v):
        d = type("d", (), {"getVar": staticmethod(lambda n: v if n == "ALP_DEVKIT_FEED" else None)})
        exec("def _f():\n" + body + "\n_f()\n", {"bb": bb, "d": d})

    with pytest.raises(_Fatal):
        run("1")
    run(None)
    run("0")
