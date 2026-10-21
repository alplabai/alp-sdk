#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Alp Lab AB
#
# Turn a bitbake deploy/ipk tree into the flat, signed dev-kit opkg feed that
# is attached to a GitHub Release (alp-sdk #2841).
#
# GitHub release assets are a flat namespace (no sub-directories), but bitbake
# writes one index per package arch. The `all` and MACHINE_ARCH dirs hold every
# layer's packages (poky, BSP, kernel modules...), so only an explicit ALLOWLIST
# of our own packages is published (--allow overrides the default). Those two
# arches are merged into ONE index; ipk filenames carry the arch, so they never
# collide. The script fails on an ipk name outside [A-Za-z0-9._-] (GitHub renames
# such assets, which would 404 the signed `Filename:` lines) and on more assets
# than a release can hold.
#
# Scope: the feed holds NO tune-arch (cortexa55/aarch64) packages, so it can only
# upgrade packages already in the image whose dependencies are already installed;
# a new RDEPENDS from the tune-arch closure cannot be resolved from it.
#
#   package_devkit_feed.sh --ipk-dir tmp/deploy/ipk --machine e1m-v2n101-a55 \
#       --tag feed-v0.17-e1m-v2n101-a55 --out feed-out [--gnupghome DIR] [--key FPR] [--allow "pkg pkg..."]
#
# Output (all in --out, upload them as release assets):
#   Packages Packages.gz Packages.sig  *.ipk   <tag>.tar.gz
# The tarball is the fallback for hosts that cannot use the release URL as an
# opkg base: unpack and `src/gz alp-devkit file:///path`. Run it where the
# private key is available (the internal runner), never in public Actions.
# Cross-platform: Linux-only by design (gpg + bash on the runner); on Windows
# use WSL. There is no native Windows equivalent.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
pubkey=$here/../keys/alp_devkit_feed_signing.pub.asc
max_assets=1000   # GitHub per-release asset limit
allow="alp-sdk alp-sdk-dev alp-devkit-feed-config"
# The example packages (alp-edgeai, ...) are tune-arch (cortexa55) and cannot be
# published until their recipes are MACHINE_ARCH (#2850 item 2).

ipk_dir= machine= tag= out= key=2B7807E20B75B471BF88FD7AC9AB032782D3143B
while [ $# -gt 0 ]; do
  case $1 in
    --ipk-dir) ipk_dir=$2; shift 2 ;;
    --machine) machine=$2; shift 2 ;;
    --tag) tag=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --key) key=$2; shift 2 ;;
    --allow) allow=$2; shift 2 ;;
    --pubkey) pubkey=$2; shift 2 ;;   # tests only; default is the committed key
    --gnupghome) export GNUPGHOME=$2; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
for v in ipk_dir machine tag out; do [ -n "${!v}" ] || { echo "missing --${v//_/-}" >&2; exit 2; }; done

arch=${machine//-/_}
# --out is wiped by design only when it is an empty/new dir: `--out .` must not
# delete the caller's working directory.
if [ -e "$out" ] && [ -n "$(ls -A "$out")" ]; then
  echo "refusing non-empty --out $out" >&2; exit 2
fi
mkdir -p "$out"
: > "$out/Packages"
for a in all "$arch"; do
  [ -f "$ipk_dir/$a/Packages" ] || { echo "no $ipk_dir/$a/Packages (run bitbake package-index)" >&2; exit 1; }
  # Keep only allowlisted stanzas, then copy exactly the ipks they name.
  awk -v allow=" $allow " 'BEGIN { RS = ""; FS = "\n"; ORS = "\n\n" }
       { for (i = 1; i <= NF; i++) if ($i ~ /^Package: /) n = substr($i, 10)
         if (index(allow, " " n " ")) print }' "$ipk_dir/$a/Packages" > "$out/Packages.part"
  while read -r f; do
    case $f in *[!A-Za-z0-9._-]*|"") echo "ipk name outside [A-Za-z0-9._-]: $f" >&2; exit 1 ;; esac
    cp "$ipk_dir/$a/$f" "$out/"
  done < <(sed -n 's/^Filename: //p' "$out/Packages.part")
  cat "$out/Packages.part" >> "$out/Packages"; rm "$out/Packages.part"
done
# Every allowlisted package must be in the merged index (a feed without alp-sdk
# would sign and publish as an empty upgrade) and every stanza needs the
# SHA256sum opkg verifies the ipk against (opkg is built with sha256).
for n in $allow; do
  grep -qx "Package: $n" "$out/Packages" || { echo "allowlisted package missing from the index: $n" >&2; exit 1; }
done
awk 'BEGIN { RS = ""; FS = "[\n]" }
     { ok = 0; for (i = 1; i <= NF; i++) if ($i ~ /^SHA256sum: [0-9a-f]{64}$/) ok = 1
       if (!ok) { print "stanza without SHA256sum: " $1 > "/dev/stderr"; bad = 1 } }
     END { exit bad }' "$out/Packages"
n_ipk=$(ls "$out"/*.ipk 2>/dev/null | wc -l)
[ "$n_ipk" -gt 0 ] || { echo "no allowlisted ipks found" >&2; exit 1; }
[ $((n_ipk + 4)) -le "$max_assets" ] || { echo "$n_ipk ipks exceed the $max_assets-asset release limit" >&2; exit 1; }
gzip -9nc "$out/Packages" > "$out/Packages.gz"
gpg --batch --yes --pinentry-mode loopback --passphrase '' \
    --local-user "$key" --detach-sign -o "$out/Packages.sig" "$out/Packages"
# Verify against the COMMITTED public key devices trust, not the signer's own
# keyring: a wrong --key would otherwise pass here and fail on every device.
vh=$(mktemp -d "${TMPDIR:-/tmp}/feed-vkey.XXXXXX")
trap 'rm -rf "$vh"' EXIT
gpg --homedir "$vh" --batch --dearmor < "$pubkey" > "$vh/pub.gpg"
gpgv --keyring "$vh/pub.gpg" "$out/Packages.sig" "$out/Packages"
tmp_tar=$(mktemp "${TMPDIR:-/tmp}/feed-tar.XXXXXX")
tar -C "$out" -czf "$tmp_tar" .
mv "$tmp_tar" "$out/$tag.tar.gz"
echo "feed ready: $out ($n_ipk ipks)"
