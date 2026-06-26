# Security Policy

## Reporting a vulnerability

Please report security vulnerabilities **privately** through GitHub's
private vulnerability reporting:

**[github.com/alplabai/alp-sdk/security/advisories/new](https://github.com/alplabai/alp-sdk/security/advisories/new)**

Do **not** open a public issue for anything you believe is a security
vulnerability — that includes weaknesses in the secure-boot chain, OTA
update verification, crypto surfaces (`<alp/security.h>`), the on-module
bridge firmware (`firmware/`), or anything that could compromise a
deployed E1M module.

You should receive an initial response within **5 business days**.
Please include enough detail to reproduce: affected SoM / SKU,
SDK version or commit, and a proof-of-concept where possible.

## Supported versions

The Alp SDK is pre-1.0.  Security fixes land on the latest tagged
release only — there are no long-term-support branches yet.

| Version        | Supported |
|----------------|-----------|
| latest release | ✅        |
| older tags     | ❌ — upgrade to the latest release |

## Scope

In scope:

- Public API surfaces under `include/alp/` and their backend
  implementations (`src/`)
- Chip drivers (`chips/`) and vendor bindings (`vendors/`)
- On-module MCU firmware (`firmware/cc3501e/`, `firmware/gd32-bridge/`)
  including the bridge wire protocol and its A/B OTA path
- Secure-boot / OTA configuration emitted from `board.yaml`
  (MCUboot, Mender, TF-M / PSA)
- Build tooling under `scripts/` to the extent it affects shipped
  firmware integrity

Out of scope:

- Vendor SDKs and silicon errata (report to the silicon vendor;
  we will coordinate where the SDK is the integration point)
- The documentation site and community forum infrastructure
- Server-side OTA infrastructure (separate project; the device-side
  trust contract is documented in
  [`docs/secure-boot.md`](docs/secure-boot.md))

## Disclosure

We follow coordinated disclosure: we ask reporters to keep findings
private until a fix is released, and we credit reporters in the
release notes unless they prefer otherwise.
