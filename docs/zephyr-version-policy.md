# Zephyr version policy

This page is the single answer to *"does alp-sdk track the latest
Zephyr, or do we own the bump cadence?"*

**Short version: we pin to the latest stable Zephyr release.  Bumps
are deliberate -- each major upgrade ships as a minor alp-sdk
release after re-verifying every wrapped peripheral on the target
SoMs.  We never track `zephyr/main` and we don't auto-merge new
upstream releases.**

## What's pinned today

| Surface                              | Pinned to                | Where                                                                                |
|--------------------------------------|--------------------------|--------------------------------------------------------------------------------------|
| Zephyr release                       | **v4.4.1** (stable)      | [`metadata/bootstrap.json`](../metadata/bootstrap.json) (`zephyr.version` -- the bootstrap-facts single source of truth, issue #917), [`west.yml`](../west.yml) (manifest), [`.github/workflows/pr-twister.yml`](../.github/workflows/pr-twister.yml), [`.github/workflows/pr-tier-a-libraries.yml`](../.github/workflows/pr-tier-a-libraries.yml), [`.github/workflows/pr-getting-started-aen801.yml`](../.github/workflows/pr-getting-started-aen801.yml) (CI), the `Zephyr-vX.Y.Z` badge in [`README.md`](../README.md), and the `ARG ZEPHYR_REV` default in [`tools/native-sim-container/Containerfile`](../tools/native-sim-container/Containerfile) (issue #1458 -- a fallback for a standalone `docker build`/`podman build` only; [`tools/native-sim-container/Makefile`](../tools/native-sim-container/Makefile)'s `build` target derives the live value from `west.yml` instead of reading this default).  (The bench itself pins its own Zephyr checkout by hand per [`HW-IN-LOOP.md`](ci/HW-IN-LOOP.md); there's no CI workflow pin for it -- CI does not drive the bench.) |
| `hal_alif` Zephyr module             | Whatever ships with the pinned Zephyr | (we do **not** re-pin -- Zephyr's own west.yml owns this revision)         |

All pins above move together when we bump.  Drift between them fails
[`scripts/check_bootstrap_manifest.py`](../scripts/check_bootstrap_manifest.py)
locally and CI on the next PR -- by design.

> **Migration note (2026-05).**  v0.5 bumps from Zephyr **v3.7.0
> LTS** to **v4.4.0** stable.  The trade is mainline-feature access
> (the LVGL v9 widget set, the new I2S `_CONTROLLER` enum spelling,
> the upstream Alif `boards/alif/ensemble_e8_dk` board files, and
> the mainline mbedtls 3.6 PSA-crypto wiring) at the cost of LTS
> stability.  Customers shipping product with a 24-month support
> window will want to re-pin alp-sdk's `west.yml` to whatever LTS
> their fleet has been signed off against -- the SDK's own
> `<alp/*>` surface stays binary-compatible.  See
> [`VERSIONS.md`](../VERSIONS.md) for the alp-sdk LTS commitment.

## When we bump

| Trigger                                       | Cadence          | Customer impact                                |
|-----------------------------------------------|------------------|------------------------------------------------|
| Zephyr **patch** within the same LTS line (e.g. 3.7.0 → 3.7.x) | Pulled into the next alp-sdk **patch release** | None visible -- no API change, no recompile needed beyond a `west update`. |
| Zephyr **LTS-to-LTS** upgrade (e.g. 3.7 → 4.x LTS)            | Triggers an alp-sdk **minor release** (e.g. v0.3 → v0.4) | API surfaces stay stable per [ADR 0001](adr/0001-wrapper-on-top-of-zephyr.md), but the underlying Kconfig + module manifest changes; consumers re-fetch via `west update`. |
| Zephyr tip (`zephyr/main`)                    | **Never used in a release.**  Investigations only. | N/A. |

We adopt patch releases freely because the wrapper at `<alp/*.h>`
absorbs the underlying Zephyr surface -- a Zephyr 3.7.1 → 3.7.2
patch never breaks `alp_i2c_open` or its callers.  LTS-to-LTS
bumps are different: Zephyr's `bt`, `wifi_mgmt`, `mqtt_client`,
and `audio_dmic` APIs have all evolved across LTS lines, and the
re-test work is significant.

## Why LTS, not the latest

Five reasons, in order of weight:

1. **Reproducibility.**  Customers building firmware on Monday and
   re-building on Tuesday should get the same binary.  Tip Zephyr
   moves daily.
2. **Vendor pack alignment.**  `hal_alif`, the Renesas RZ/V2N AI
   SDK pack, the DEEPX DXNN host SDK, and the NXP i.MX 93 AI SDK
   all align release cadence to **a Zephyr LTS line**, not to
   Zephyr's monthly RCs.  Tracking tip breaks the pack we depend on.
3. **CI cost.**  Every Zephyr bump invalidates the `actions/cache`
   build artefacts under `~/zephyrproject` -- a clean rebuild adds
   ~5 min per PR.  The cache key embeds the full `X.Y.Z`
   (`zephyr-v4.4.1-${{ runner.os }}`), so a patch bump forces the same
   clean rebuild a minor bump does -- there is no tier that stays
   within the cache key.
4. **Customer support window.**  Per [`VERSIONS.md`](../VERSIONS.md),
   alp-sdk v1.0 carries a 24-month LTS commitment.  That commitment
   only holds if the Zephyr line underneath it is also under LTS
   support from the upstream project.
5. **Stability for shipping product.**  Customers shipping E1M
   modules to production cannot retest their firmware monthly.
   The LTS contract is the predictability they pay for.

## The bump procedure (for maintainers)

When a new Zephyr LTS lands and we want to adopt it:

1. **Open a tracking issue** at `alplabai/alp-sdk` titled
   `Zephyr <new-LTS>: bump alp-sdk minor`.
2. **Verify vendor packs.**  Confirm `hal_alif`, Renesas RZ/V2N AI
   SDK, DEEPX DXNN, and NXP i.MX 93 AI SDK ship a revision that
   targets the new Zephyr LTS.  If any is lagging, defer.
3. **Branch + bump all the pins together** in a single PR:
   - Edit `metadata/bootstrap.json` &mdash; `zephyr.version` (the pin's
     single source, issue #917).
   - Run `python3 scripts/check_bootstrap_manifest.py --fix` to propagate
     that one edit to every dependent machine-pin site:
     - `west.yml` &mdash; `projects.zephyr.revision`
     - `.github/workflows/pr-twister.yml` &mdash; `--mr` arg + cache key
     - `.github/workflows/pr-tier-a-libraries.yml` &mdash; `--mr` arg + cache key
     - `.github/workflows/pr-getting-started-aen801.yml` &mdash; cache key
       (does **not** track the separate `ZEPHYR_SDK_VERSION` toolchain pin)
     - `README.md`'s `Zephyr-vX.Y.Z` badge
     - every `metadata/libraries/*.yaml` manifest that is a genuine
       in-tree Zephyr subsystem (`integration.zephyr.module: null` **and**
       `requires.os == [zephyr]` -- today that's `coap.yaml`, `lwm2m.yaml`,
       `modbus.yaml`) &mdash; its `version:` field
     - `tools/native-sim-container/Containerfile` &mdash; the `ARG ZEPHYR_REV`
       default (issue #1458; a fallback for a standalone `docker
       build`/`podman build` only -- `tools/native-sim-container/Makefile`'s
       `build` target derives the live value from `west.yml` and never reads
       this default)
   - Run `python3 scripts/check_bootstrap_manifest.py` with no flag to
     prove every pin above now agrees with `metadata/bootstrap.json` --
     it fails loudly on any pin left behind.
   - Prose docs, CHANGELOG history, and every other `metadata/libraries/*.yaml`
     manifest (anything pinning its own upstream release/SHA, plus every
     manifest's `# Grounding (pinned Zephyr ...)` provenance comment and
     `$ZEPHYR_BASE/...:<line>` citations, which record where a symbol was
     READ and stay frozen at the old version) are **not** `--fix` sites --
     update them by hand.
4. **Re-verify the peripheral matrix** -- every column in
   [`docs/os-support-matrix.md`](os-support-matrix.md) re-runs on
   native_sim (CI, `pr-twister.yml`).  Real-silicon columns re-run as
   an **explicitly-invoked bench run** under a held labgrid
   reservation, not a nightly CI job -- see
   [`docs/ci/HW-IN-LOOP.md`](ci/HW-IN-LOOP.md) for that contract; the
   bump PR (or a follow-up before the tag) attaches the bench result.
   Twister failures get peripheral-by-peripheral triage.

Steps 5-7 below apply to an **LTS-to-LTS / minor** bump only -- a same-line
**patch** bump (e.g. 4.4.0 -> 4.4.1) stops after step 4: no
`metadata/sdk_version.yaml` change, no CHANGELOG `[Unreleased] -- vX.Y.0
candidate` heading, and no tag/release (see the "When we bump" table above).

5. **Update `metadata/sdk_version.yaml`** with the new minor.
6. **CHANGELOG entry** under
   `[Unreleased] -- v0.<minor>.0 candidate` calling out the
   Zephyr LTS bump + any user-visible Kconfig changes.
7. **Tag + release.**  v0.4-Zephyr3.7 retires the day v0.5-Zephyr4.4
   ships; old tags remain available for customers who can't migrate
   immediately.

## When the customer asks

> *"Do you always update Zephyr in alp-sdk and bump alp-sdk
>  every time?"*

**Answer**: No.  alp-sdk pins a Zephyr LTS release.  Patch updates
within that LTS roll into our patch releases automatically.
LTS-to-LTS upgrades are an explicit, version-bumped event with
full peripheral re-verification.  You upgrade Zephyr when you
upgrade alp-sdk; you don't track them separately.

## See also

- [`west.yml`](../west.yml) -- the manifest the pin lives in.
- [`ADR 0001 -- Wrapper on top of Zephyr`](adr/0001-wrapper-on-top-of-zephyr.md)
  -- the "wrapper absorbs upstream churn" boundary.
- [`VERSIONS.md`](../VERSIONS.md) -- alp-sdk's own version roadmap +
  the v1.0 LTS commitment.
- [`docs/os-support-matrix.md`](os-support-matrix.md) -- the matrix
  the bump procedure re-verifies.
