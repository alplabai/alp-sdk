# CC3501E prebuilt firmware — release notes

Each entry corresponds to a tagged release of the in-tree
[`firmware/cc3501e/`](..) source.  The signed binary, its detached
signature, and a SHA-256 manifest are dropped into this directory and
named `cc3501e-vX.Y.Z.bin` (matching `firmware/cc3501e/firmware-version.txt`).

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

No prebuilt binaries yet.  The in-tree `firmware/cc3501e/` v0.1 source
has landed (silicon-free core + stub backend + native transport test);
the first signed binary is built on the bench with TI `ticlang` + the
SimpleLink CC33xx SDK (the `ti` backend).

When the first firmware release is built + signed, it will populate this
directory with:

- `cc3501e-v0.1.0.bin`         -- signed firmware image
- `cc3501e-v0.1.0.bin.sig`     -- detached Ed25519 signature
- `cc3501e-v0.1.0.bin.sha256`  -- integrity manifest

and an entry below describing the included feature scope (the v0.1 META
group: PING + GET_VERSION + GET_MAC + RESET; Wi-Fi station lands in v0.2).
