# Alif Ensemble SVD register descriptions (vendor data under its own terms)

The files in this directory are **Alif Semiconductor's**, carried under
Alif's own licence ([`License.txt`](License.txt), unmodified), **not** this
repository's Apache-2.0. See [ADR 0032](../../../docs/adr/0032-vendor-data-under-its-own-terms.md)
for why vendor data lives in this segregated subtree, and the root
[`NOTICE`](../../../NOTICE) entry that names it.

| File | Core | SHA-256 |
| --- | --- | --- |
| `AE822FA0E5597BS0_CM55_HE_View.svd` | Ensemble E8 M55-HE | `f1b702c5dcfd69336c10f5ef4f9892ecfd1c3333a113b4bff0bfc2380ed8e53a` |
| `AE822FA0E5597BS0_CM55_HP_View.svd` | Ensemble E8 M55-HP | `a021d86c2413eb17a2b558ec66a5bcd79f9032cbf441422f87d97afd142f2bb5` |
| `License.txt` | Alif Software License Agreement | `f1d598c22f2bab994c12b4e2968c67651bae0679c61d24523ab76227966a4f58` |

**Source:** [`alifsemi/alif_ensemble-cmsis-dfp`](https://github.com/alifsemi/alif_ensemble-cmsis-dfp)
at commit `b83f294445fe6496c8c5dda136085f5c12c8f8cc` (2026-02-11),
`Debug/SVD/` and `License.txt`, byte-identical to that commit.

**Why BS0 files for an LS0 part.** The E1M-AEN modules carry
`AE822FA0E5597LS0`. The DFP ships no LS0-named SVD; its own pack description
(`AlifSemiconductor.Ensemble.pdsc`, device `AE822FA0E5597LS0`) maps both
cores of that part to these `AE822FA0E5597BS0_*_View.svd` files. So this is
the vendor's mapping for our part, not a substitution of another variant's
register map.

**Field-of-use restriction (licence condition 4):** use is restricted to Alif
Semiconductor silicon.

**What uses them:** `metadata/socs/alif/ensemble/e8.json`
`variants[].debug.svd`, keyed by core id, which debug tooling passes as
cortex-debug's `svdFile` for the Cortex Peripherals register view.
