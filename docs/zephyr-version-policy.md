# Zephyr version policy

This page is the single answer to *"does alp-sdk track the latest
Zephyr, or do we own the bump cadence?"*

**Short version: we pin to a Zephyr LTS release.  Bumps are
deliberate -- each LTS-to-LTS upgrade ships as a minor alp-sdk
release after re-verifying every wrapped peripheral on the target
SoMs.  We never track `zephyr/main` and we don't auto-merge new
upstream releases.**

## What's pinned today

| Surface                              | Pinned to        | Where                                                                                |
|--------------------------------------|------------------|--------------------------------------------------------------------------------------|
| Zephyr release                       | **v3.7.0 LTS**   | [`west.yml`](../west.yml) (manifest), [`.github/workflows/pr-twister.yml`](../.github/workflows/pr-twister.yml) (CI), [`.github/workflows/nightly-aen-hil.yml`](../.github/workflows/nightly-aen-hil.yml) (HIL) |
| Zephyr CI docker image               | `v0.27.4`        | [`.github/workflows/pr-twister.yml`](../.github/workflows/pr-twister.yml)             |
| `hal_alif` Zephyr module             | Whatever ships with the pinned Zephyr | (we do **not** re-pin -- Zephyr's own west.yml owns this revision)         |

All three pins move together when we bump.  Drift between them
fails CI on the next PR -- by design.

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
   ~5 min per PR.  Patch bumps stay within the cache key
   (`zephyr-v3.7.0-${{ runner.os }}`); minor bumps blow it away
   intentionally.
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
3. **Branch + bump the three pins together** in a single PR:
   - `west.yml` &mdash; `projects.zephyr.revision`
   - `.github/workflows/pr-twister.yml` &mdash; `--mr` arg + cache key
   - `.github/workflows/nightly-aen-hil.yml` &mdash; `--mr` arg
   - `.github/workflows/pr-twister.yml`'s docker `image:` tag (the
     Zephyr `ci:vX.Y.Z` image tracks the LTS line).
4. **Re-verify the peripheral matrix** -- every column in
   [`docs/os-support-matrix.md`](os-support-matrix.md) re-runs
   either on native_sim (CI) or on real hardware (nightly HIL).
   Twister failures get peripheral-by-peripheral triage.
5. **Update `metadata/sdk_version.yaml`** with the new minor.
6. **CHANGELOG entry** under
   `[Unreleased] -- v0.<minor>.0 candidate` calling out the
   Zephyr LTS bump + any user-visible Kconfig changes.
7. **Tag + release.**  v0.3-Zephyr3.7 retires the day v0.4-Zephyr4.x
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
