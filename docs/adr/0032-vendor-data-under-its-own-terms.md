# 0032. Vendor data carried under its own non-Apache terms lives in a segregated subtree, not the curated-library allowlist

Status: Proposed
Date: 2026-08-30
Deciders: alpCaner

## Context

**Terminology, because this repo already uses the phrase the other way.**
Everywhere else, "licence-gated" means *we do not redistribute it; obtain it
from the vendor* -- `docs/ci/HW-IN-LOOP.md:12` ("SETOOLS is license-gated and
must not be redistributed"), `docs/getting-started.md:147`,
`docs/adr/0021-toolchain-provisioning.md:428`. This ADR is about the
**opposite** case: vendor data that the vendor *does* permit us to
redistribute, but under its own terms rather than the repo's Apache-2.0.

Rather than invert an established term, that case is named **vendor data under
its own terms** throughout — in the title, the filename and the index row. The
rename happened before this landed, deliberately: ADRs here are append-only
(`docs/adr/README.md:10-12`), so a filename carrying the inverted sense would
have been permanent. "Licence-gated" keeps its existing meaning everywhere,
including in this document.


[#948](https://github.com/alplabai/alp-sdk/issues/948) wants the Alif Ensemble
SVD files in tree so `cortex-debug` can populate its Cortex Peripherals view.
The issue thread converged on a six-item acceptance list whose first item is
"add `LicenseRef-Alif` to `metadata/schemas/library-v1.schema.json`
`license.enum`". Working through it against the tree, two things do not hold.

**The allowlist is the wrong gate.** `library-v1.schema.json` `license.enum`
governs `metadata/libraries/*.yaml` — the 35 library manifests under `metadata/libraries/` a project pulls
in through a `libraries:` selection. Its own description states what it is for:
a GPL-family or proprietary licence is rejected "so a copyleft surprise cannot
ride in through a `libraries:` selection (ADR 0018 non-goal)". An SVD is not
west-pulled, is not selectable, and is not a library. Nothing about it passes
through that schema; `soc-spec-v1.schema.json`, which describes the SoCs the
files belong to, carries no `license` field at all. Adding `LicenseRef-Alif` to
the library allowlist would declare Alif-SLA content selectable **as a curated
library** — widening the gate 0018 built, in order to authorise something that
never passes through it.

**The root `NOTICE` already says the opposite, in public.** Its "Vendor BSPs and
SDK binaries" paragraph reads, verbatim:

```text
Silicon-vendor BSPs and SDK binaries (Alif Ensemble, Renesas RZ/V2N, NXP
i.MX93, DEEPX DX-M1) are NOT redistributed in this repository. They are
obtained directly from the vendor under the vendor's own license. The
vendors/<name>/ directories here contain only Alp Lab's own integration
shims (Apache-2.0) plus instructions; see each vendors/<name>/README.md.
```

Vendoring an Alif file contradicts that sentence. So the work is not "create a
`LICENSES/` directory" — it is **amending a licence representation this
repository has already published**, which is a different kind of change and a
different kind of sign-off.

The vendor terms themselves are permissive enough to make the question live
rather than academic. Alif's `License.txt` in
[`alifsemi/alif_ensemble-cmsis-dfp`](https://github.com/alifsemi/alif_ensemble-cmsis-dfp)
permits redistribution in source and binary form provided condition 1 is met
(retain the copyright notice, the full condition list and the disclaimers).
Condition 4 restricts the field of use to Alif silicon, and condition 5 forbids
subjecting the software to a copyleft licence. Apache-2.0 is not copyleft, so
this repository is compliant as it stands — but the file cannot be **covered
by** Apache-2.0, which is exactly what a root `LICENSE` with no carve-out
implies.

[#383](https://github.com/alplabai/alp-sdk/issues/383) (Memfault) is the same
question from the other side: a proprietary licence correctly refused entry to
the allowlist, for a component that genuinely *is* a curated library. The two
issues have been read as needing "the same mechanism". They do not, and this
ADR exists to say which mechanism each one needs.

## Decision

**1. The curated-library allowlist stays permissive-only, and is not the
carriage mechanism for vendor data.** `library-v1.schema.json` `license.enum`
is extended only for licences of components that are actually selected through
`libraries:`. #948 needs no entry there; acceptance item 1 as written is
withdrawn. #383 is unaffected — memfault-firmware-sdk *is* a curated library,
so its `LicenseRef-Memfault` question stands on its own merits and still needs
the legal review 0018 requires.

**2. Vendor data under its own terms is carried in a segregated subtree.** The subtree is `metadata/svd/<vendor>/` for vendor register
descriptions (per #948), and `vendors/<name>/` for vendored source. That choice
is load-bearing, not cosmetic: `scripts/check_public_private.py:76-82` lists
`vendors` in `DEFAULT_EXCLUDES`, so a `vendors/` subtree is never scanned for
SoM/lab-IP leaks, while `metadata/` is a default scan root (`:60-73`) and *is*
scanned. Vendor SVDs are dense register and pad detail, so they belong on the
scanned side. When such data lands, it lands as: the data in its own directory, the
vendor's **unmodified** licence file beside it, and a README recording the
vendor, the exact upstream repository and commit the bytes came from, and the
field-of-use restriction. The root `NOTICE` gains an entry naming the component
and its licence.

**This is convention, not enforcement, and that is a known cost.** Nothing in
`scripts/` or `.github/workflows/` reads `NOTICE` today, which is how the seven
missing entries above accumulated unnoticed. Closing that needs a
`check_notice_vendors.py` asserting every vendored subtree with a licence file
has a `NOTICE` entry, landed across the four sites `adding-a-ci-gate` requires.
Until it exists, Decision 3 is enforced by review alone. The
licence attaches to the subtree, not to a metadata key — which is why no schema
change is required to carry it.

**3. The root `LICENSE` stops implying whole-tree coverage, in the same change
as the first file that needs the carve-out.** `NOTICE`'s "Vendor BSPs and SDK
binaries" paragraph is amended to separate two cases it currently collapses:
vendor **BSPs and SDK binaries**, which remain not redistributed and are
obtained from the vendor; and vendor **register-description data**, which may
be redistributed under the vendor's own terms where the vendor permits it. The
amendment never lands ahead of the file it describes — a `NOTICE` that
describes an empty directory is a false statement in the same way the current
one would be after a silent vendoring.

This ADR records the **mechanism**, not an authorisation. Whether Alp Lab
redistributes any particular vendor's files under that vendor's terms remains a
maintainer decision per vendor, and nothing here is legal advice.

## Alternatives

- **Extend `library-v1.schema.json` `license.enum` with `LicenseRef-Alif`.**
  The acceptance list's original item 1. Rejected above: it widens the ADR 0018
  guard to admit a licence class into a selection path the data never travels.
  The strongest argument for it was "there is already an enum, use it" — which
  is reuse of a name, not of a mechanism.
- **Add a `license` field to `soc-spec-v1.schema.json`.** Rejected: no consumer
  reads it, which is precisely the defect [#1026](https://github.com/alplabai/alp-sdk/issues/1026)
  recorded against #987's debug block; and the licence attaches to a file, not
  to the SoC that file describes. One SoC's SVD and its Renode descriptor can
  carry different terms.
- **Take the bytes from `cmsis-svd/cmsis-svd-data`, which carries a root
  `LICENSE-APACHE`.** Rejected twice over. A third party mirroring a file
  cannot widen the grant the original author gave, and the aggregator's own
  README says so ("Under data, the license from each Vendor is provided along
  with the SVDs from that vendor"). Independently, the mirror is incomplete for
  the parts this SDK models — including `AE822FA0E5597LS0`, the E1M-AEN801 part
  on the bench, so the one acceptance criterion that matters could not be
  demonstrated from it.
- **Do not redistribute at all; resolve a customer-supplied path.**
  `variants[].debug.svd` as a bare relative path resolved repo-directory-first
  then `ALP_SVD_DIR`, modelled on the `SETOOLS_DIR` pattern this repo already
  uses for genuinely licence-gated vendor tooling. Not rejected on merit: it
  needs no licence work at all, half of it has already shipped as
  `tan debug-config --svd`, and it is the fallback if the per-vendor
  redistribution decision goes the other way. It is also what defines the
  **absence semantics** an implementer needs and this ADR would otherwise leave
  unstated: when the subtree is absent and nothing resolves, the consumer omits
  `svdFile` rather than failing the session — an unresolvable SVD costs only the
  peripheral view.
- **Put the bytes in `alp-sdk-internal`.** `docs/ci/runner-architecture.md:113`
  records that repo as the existing home for material that cannot be public.
  Rejected for this case specifically: the Alif terms permit public
  redistribution, so moving the files private would withhold from customers
  something the vendor allows them to have, and would leave the public SDK
  unable to populate the peripheral view out of the box — the entire point of
  #948.
- **Adopt full REUSE compliance.** The tidy answer, and the one a licence
  scanner would prefer. Rejected for now: the benefit is tooling nobody here
  runs. (The cost is smaller than it first appears -- 587 of the 612 `.c`/`.h`/
  `.py` files under `src/`, `include/` and `scripts/` already carry
  `SPDX-License-Identifier` -- so the honest reason is the absent consumer, not
  the effort.) The **subtree + licence file + README** half of the pattern below
  is already in use for every existing third-party component. The `NOTICE` half
  is **not**: of the 14 directories under `vendors/`, seven vendored-source
  components have no `NOTICE` entry at all (`catch2`, `doctest`, `etl`, `fmt`,
  `jsmn`, `minimp3`, `u8g2`; `vendors/etl/` alone is 363 tracked files, 361 of them headers).
  `deepx-dxm1` and `nxp-imx93` have no by-name entry either but are covered by
  the "Vendor BSPs and SDK binaries" paragraph. So Decision 3 is repairing an
  existing drift, not merely continuing a practice.

## Consequences

Good:

- #948 loses its hardest stated prerequisite. The remaining blocker is the
  per-vendor redistribution decision, not a schema change that would have had
  to be argued past ADR 0018.
- ADR 0018's guard keeps meaning what it says. "Everything selectable through
  `libraries:` is permissively licensed" stays true, rather than becoming
  "…except the entries added for things that are not libraries".
- The two issues stop being coupled. #383 no longer waits on #948's outcome and
  #948 no longer inherits #383's legal review.

Bad / costs:

- The repository grows a second licence regime, and a reader can no longer
  answer "what licence is this file under?" from the root `LICENSE` alone. That
  is inherent to carrying vendor data at all; the mitigation is that the
  regime is confined to a subtree that says so in its own README.
- The field-of-use restriction (Alif condition 4) becomes something downstream
  users inherit without opting in. It is compatible with what an SDK for Alif
  silicon is for, but it is a real narrowing and belongs in the `NOTICE` entry
  rather than only here.
- The `NOTICE` amendment is coupled to the first vendored file, so it cannot be
  landed early as pure plumbing. This ADR is the part that lands early.

## Relationship to earlier ADRs

Extends [0018](0018-curated-third-party-libraries.md) rather than superseding
it: 0018 decided what may enter the curated-library set and on what licence
terms, and this ADR decides where everything *else* with third-party terms
goes.

It does not supersede 0018, but it does **narrow one clause**. 0018's non-goals
(`0018:146-148`) include "redistributing licence-gated SDKs or binaries
(manifests may *reference* a vendor download the customer performs -- same rule
as BSPs)". That non-goal covers material the vendor does not permit us to
redistribute, which remains out of scope and unchanged. This ADR covers vendor
data that *is* redistributable under its own terms -- a case 0018 did not
address either way.
