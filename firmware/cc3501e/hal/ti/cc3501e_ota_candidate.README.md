<!-- SPDX-License-Identifier: Apache-2.0 -->
# `cc3501e_ota_candidate.c` — internal-only artifact

`cc3501e_ota_candidate.c` is **not** committed to this public repository. It is a
generated, **signed firmware binary** (a `bin2c` C byte-array of the plain, radio-free
`cc3501e-bridge` image in TI GPE format) used only by the `--ota-selftest` build to
validate the OTA stage → swap → boot cycle.

It lives in the private `alp-sdk-internal` repo because it is a prebuilt firmware
**binary blob** built through the license-gated TI SimpleLink SDK toolchain — binary
blobs (FIP / BL2 / `.mot` / `.wic` / a C-array of the same) are internal-class per the
project's public/internal classification. The bridge *source* in this tree is public;
only this compiled+signed artifact is withheld. See alp-sdk issue #590.

## Building the OTA self-test

The default (customer) build never references this file. Only `--ota-selftest` does.
Before an OTA self-test build, stage the artifact from `alp-sdk-internal` into this
directory:

```
cp <alp-sdk-internal>/firmware/cc3501e/hal/ti/cc3501e_ota_candidate.c \
   firmware/cc3501e/hal/ti/cc3501e_ota_candidate.c
```

`build_ti.sh --ota-selftest` (and `build_ti.ps1 -OtaSelftest`) fail with a clear
message if the file is absent. The staged copy is git-ignored.

## Regenerating it

`build_ti.sh` (plain) → `flash-images-builder build+sign vendor_image --version <higher
than the flashed primary>` → `bin2c`. The regenerated artifact is committed to
`alp-sdk-internal`, not here.
