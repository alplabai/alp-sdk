<!--
SPDX-License-Identifier: Apache-2.0

Thanks for contributing to alp-sdk.  Please replace each
placeholder below with the relevant detail and delete the
guidance comments before submitting.
-->

## Summary

<!-- One paragraph: what this PR changes and why. -->

## Roadmap row

<!-- Which row in VERSIONS.md does this PR implement?  Quote the
     bullet, e.g.:

       v0.2 / Chips: alp_ov5640 (MIPI CSI camera)

     If the PR doesn't map to an existing row, link the issue
     where the row was discussed. -->

## Scope

- [ ] Public API change (`include/alp/**`)
- [ ] Internal-only change (`src/**`, `chips/**`, `vendors/**`)
- [ ] Documentation only (`docs/**`, `README.md`, `VERSIONS.md`, etc.)
- [ ] Build / CI (`CMakeLists.txt`, `west.yml`, `.github/**`, `docs/ci/**`)
- [ ] Metadata (`metadata/**`)
- [ ] Tests / examples (`tests/**`, `examples/**`)

## ABI impact

<!-- Pre-1.0: any change to <alp/**> headers should be flagged.
     Post-1.0: breaking changes require a major bump.  -->
- [ ] No public-header change.
- [ ] Public-header addition (additive only — fine pre-1.0, fine for minor bump post-1.0).
- [ ] Public-header *removal* or signature change — requires deprecation cycle once we ship v1.0.

## Test plan

- [ ] Twister green on `native_sim/native/64`:
      `EXTRA_ZEPHYR_MODULES=$(pwd) python3 zephyr/scripts/twister --testsuite-root tests/zephyr --testsuite-root examples -p native_sim/native/64`
- [ ] Metadata validates: `python3 scripts/validate_metadata.py`
- [ ] (HW-in-loop) flashed and ran on a real EVK — paste the captured log.

## Checklist

- [ ] CHANGELOG.md updated under `[Unreleased]`.
- [ ] If this changes the public surface, `VERSIONS.md` reflects the new row / acceptance bar.
- [ ] If this adds a chip driver, the symbols use the chip's natural prefix (no `alp_` on third-party silicon).
- [ ] If this adds a SoC, `metadata/socs/<vendor>/<family>/<part>.json` validates and references the right datasheet revision.
