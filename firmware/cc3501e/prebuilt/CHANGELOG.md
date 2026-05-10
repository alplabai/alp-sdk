# CC3501E prebuilt firmware — release notes

Each entry corresponds to a tagged release of the
`alplabai/cc3501e-firmware` repository.  The signed binary, its
detached signature, and a SHA-256 manifest are dropped into this
directory and referenced by `firmware/cc3501e/protocol-version.txt`.

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

No prebuilt binaries yet.  The CC3501E firmware repo
(`alplabai/cc3501e-firmware`) has not been created.  The
alp-sdk-side scaffolding (wire protocol header, SPI client,
bridge architecture doc, `flash.py` stub) is ready to receive
the first release.

When the first firmware release lands, it will populate this
directory with:

- `cc3501e-v0.1.0.bin`         -- signed firmware image
- `cc3501e-v0.1.0.bin.sig`     -- detached Ed25519 signature
- `cc3501e-v0.1.0.bin.sha256`  -- integrity manifest

and an entry below describing the included feature scope (Wi-Fi
station + PING + GET_VERSION expected for v0.1).
