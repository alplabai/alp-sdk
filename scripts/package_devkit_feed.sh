#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Alp Lab AB
#
# Turn a bitbake deploy/ipk tree into the flat, signed dev-kit opkg feed that
# is attached to a GitHub Release (alp-sdk #2841).
#
# GitHub release assets are a flat namespace (no sub-directories), but bitbake
# writes one index per package arch. A dev-kit feed only needs the arches our
# own layers publish -- `all` and the MACHINE arch -- so this merges those into
# ONE index. ipk filenames carry the arch, so they never collide.
#
#   package_devkit_feed.sh --ipk-dir tmp/deploy/ipk --machine e1m-v2n101-a55 \
#       --tag feed-v0.17-e1m-v2n101-a55 --out feed-out [--gnupghome DIR] [--key FPR]
#
# Output (all in --out, upload them as release assets):
#   Packages Packages.gz Packages.sig  *.ipk   alp-devkit-feed-<tag>.tar.gz
# The tarball is the fallback for hosts that cannot use the release URL as an
# opkg base: unpack and `src/gz alp-devkit file:///path`. Run it where the
# private key is available (the internal runner), never in public Actions.
set -euo pipefail

ipk_dir= machine= tag= out= key=2B7807E20B75B471BF88FD7AC9AB032782D3143B
while [ $# -gt 0 ]; do
  case $1 in
    --ipk-dir) ipk_dir=$2; shift 2 ;;
    --machine) machine=$2; shift 2 ;;
    --tag) tag=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --key) key=$2; shift 2 ;;
    --gnupghome) export GNUPGHOME=$2; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
for v in ipk_dir machine tag out; do [ -n "${!v}" ] || { echo "missing --${v//_/-}" >&2; exit 2; }; done

arch=${machine//-/_}
rm -rf "$out"; mkdir -p "$out"
: > "$out/Packages"
for a in all "$arch"; do
  [ -f "$ipk_dir/$a/Packages" ] || { echo "no $ipk_dir/$a/Packages (run bitbake package-index)" >&2; exit 1; }
  cp "$ipk_dir/$a"/*.ipk "$out/"
  cat "$ipk_dir/$a/Packages" >> "$out/Packages"
done
gzip -9nc "$out/Packages" > "$out/Packages.gz"
gpg --batch --yes --pinentry-mode loopback --passphrase '' \
    --local-user "$key" --detach-sign -o "$out/Packages.sig" "$out/Packages"
gpg --batch --verify "$out/Packages.sig" "$out/Packages"
tmp_tar=$(mktemp)
tar -C "$out" -czf "$tmp_tar" .
mv "$tmp_tar" "$out/alp-devkit-feed-$tag.tar.gz"
echo "feed ready: $out ($(ls "$out"/*.ipk | wc -l) ipks)"
