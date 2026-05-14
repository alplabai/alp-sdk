# Contributing to alp-sdk-community (Tier 2)

The [alplabai/alp-sdk-community](https://github.com/alplabai/alp-sdk-community)
repo is where community-contributed chip drivers and libraries live.
This page walks through the lifecycle: from "I have a chip driver"
to "customers can drop it into their workspace."

> **TL;DR.**  Fork the template, fill in `metadata.yaml`, drop your
> driver source under `chips/<name>/` or `libraries/<name>/`, get
> `pr-build-contribution` + `pr-metadata-validate` + `pr-lint`
> green, open a PR.

## The three tiers

```
┌──────────────────────────────────────────────────────────────────┐
│ Tier 1: alp-sdk (this repo)                                      │
│   80 chips, 25 libraries.  Maintainer-curated, Apache-2.0,       │
│   portability-tested, doxygen-clean, ABI-tracked, CHANGELOG'd.   │
└──────────────────────────────────────────────────────────────────┘
┌──────────────────────────────────────────────────────────────────┐
│ Tier 2: alplabai/alp-sdk-community  ← you are here               │
│   Community-contributed.  Apache/MIT/BSD only.  Standard         │
│   template + metadata.yaml.  CI-builds-clean for native_sim.     │
│   Per-contribution CODEOWNERS = author.                          │
└──────────────────────────────────────────────────────────────────┘
┌──────────────────────────────────────────────────────────────────┐
│ Tier 3: customer / private repos                                 │
│   Your own drivers that don't make sense upstream.  Same shape;  │
│   consumed via EXTRA_ZEPHYR_MODULES or your own west.yml.        │
└──────────────────────────────────────────────────────────────────┘
```

Tier 2's quality bar is intentionally lower than Tier 1 -- the
goal is to let customers reach hundreds of community chip drivers
without depending on the maintainer's review bandwidth.  In
return, Tier 2 contributions are:

- Single-author by default (the contribution's `author:` field
  in `metadata.yaml` is auto-assigned as the CODEOWNER for that
  one path).
- Build-clean on `native_sim/native/64` but not required to pass
  HiL verification.  They ship with the `[UNTESTED]` badge unless
  you can demonstrate silicon bring-up.
- Stable enough to compile, but no ABI-snapshot tracking.  Bumping
  your `abi_version:` field when you break the public header is
  on you.

## Per-contribution checklist

A new chip driver under `alp-sdk-community/chips/foo/` needs:

```
chips/foo/
├── README.md                # required: usage + author + datasheet refs
├── CMakeLists.txt           # required: alp-sdk-style
├── include/foo.h            # required: public API
├── src/foo.c                # implementation
├── metadata.yaml            # required: see below
└── samples/foo-basic/       # ≥ 1 sample app required
    ├── CMakeLists.txt
    ├── prj.conf
    └── src/main.c
```

A new library under `alp-sdk-community/libraries/foo/` has the
same shape minus the `include/` distinction (libraries usually
ship their own include layout).

## metadata.yaml schema

The schema file lives in alp-sdk at
[`metadata/schemas/contribution-v1.schema.json`](../metadata/schemas/contribution-v1.schema.json).
A minimal `metadata.yaml`:

```yaml
schema_version: 1
name: bme680
kind: chip
family: sensor
interfaces: [i2c]
description: "Bosch BME680 air-quality + temp + pressure + humidity"
license: Apache-2.0
author: "@yourgh"
abi_version: "0.1"
dependencies:
  - { repo: alp-sdk, version: ">=0.5.0" }
verification:
  hil_silicon: untested
  smoke_tests: null_arg_guard
upstream_status: first-class community
```

## Quality gate (CI)

`alp-sdk-community/.github/workflows/pr-build-contribution.yml`
runs per touched contribution:

1. **`pr-metadata-validate`** -- `metadata.yaml` validates against
   `metadata/schemas/contribution-v1.schema.json` (alp-sdk side).
2. **`pr-build-contribution`** -- `west build -b native_sim/native/64
   samples/<first-sample>` compiles clean (warnings tolerated;
   errors fail).
3. **`pr-lint`** --
   - `clang-format-diff` on changed lines (style warnings, not
     errors).
   - Apache-2.0 / MIT / BSD license-header check on every `.c` /
     `.h` (rejects GPL).
   - `registry.yaml` entry exists for the touched contribution
     and matches `metadata.yaml`'s `name`/`kind`/`family`.

## Customer integration patterns

Customers consume Tier 2 contributions in three ways.  Pick the
one that fits.

### Pattern A: pull-everything (broadest reach)

```yaml
# In customer's workspace, add to west.yml:
manifest:
  projects:
    - name: alp-sdk-community
      url: https://github.com/alplabai/alp-sdk-community
      revision: main
      groups: [community]
```

```bash
west update --group-filter +community
```

### Pattern B: per-contribution selection (cleaner)

```yaml
# Add the specific contributions to your west.yml's import:
manifest:
  imports:
    - file: west.yml
      name-allowlist:
        - chip-bme680
        - lib-modbus
```

### Pattern C: search-then-clone (offline workflows)

```bash
gh repo view alplabai/alp-sdk-community --json file:registry.yaml
# Browse, clone the whole repo, copy contributions in manually.
```

## "Verified" promotion path

A Tier 2 contribution becomes a candidate for Tier 1 promotion
when:

- It's used in at least one alp-sdk customer's shipping product
  (verifiable via the verification ledger in
  `docs/test-plan.md`).
- HiL evidence captured under the contribution's `samples/`.
- 6+ months of stable use without ABI-breaking changes.
- Author agrees to transfer maintenance to alplabai/*.

Promotion lands as an alp-sdk PR that:

- Copies the contribution from `alp-sdk-community/chips/<name>/`
  to `alp-sdk/chips/<name>/`.
- Adds the chip to `metadata/chip-registry.yaml`.
- Adds a CHANGELOG entry.
- The original alp-sdk-community entry stays but its
  `registry.yaml` row gains `promoted_to: alp-sdk` to redirect
  customers.

## Templates

`cp -r templates/chip-skeleton chips/foo` (in alp-sdk-community)
gives you a minimal but valid Tier 2 chip driver to fill in.
Same for `templates/library-skeleton`.
