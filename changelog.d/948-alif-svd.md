### Added — Alif Ensemble E8 SVD files, so debuggers get a peripheral/register view (#948)

`metadata/svd/alif/` now carries Alif's CMSIS-SVD register descriptions for
the Ensemble E8 M55-HE and M55-HP cores, copied unmodified from
`alifsemi/alif_ensemble-cmsis-dfp` at `b83f2944`. The files are vendor data
under Alif's own licence, not Apache-2.0. Following ADR 0032, Alif's
unmodified `License.txt` sits beside them, with a README recording
provenance, SHA-256 digests and the field-of-use restriction (Alif silicon
only), and the root `NOTICE` names the exception.

`alif/ensemble/e8.json` declares `variants[].debug.svd` for `m55_hp` and
`m55_he`, which tooling passes as cortex-debug's `svdFile`. The files are
named `AE822FA0E5597BS0_*`: the DFP's own pack description maps the
modules' `AE822FA0E5597LS0` part to exactly these files, so this is the
vendor's mapping, not a substitution of another variant's register map.
`validate_metadata.py` now checks that both files exist.
