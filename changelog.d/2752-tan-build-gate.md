### Added — per-PR `tan build` gate (`pr-tan-build.yml`) (#2752)

Nothing compiled an example through `tan build` on a pull request:
`onramp-clean-container.yml` is docs-path filtered with its real-ELF job
label-gated, and tan-cli's seam2 runs post-merge, so a break in the customer
build path surfaced only after it landed. `pr-tan-build.yml` installs the
pinned tan-cli release (`TAN_REF`, currently `v0.6.0`; it must move with the
tan release) and builds one Zephyr example per SoM family through
`tan build --project <ex> --sdk-root "$PWD" --format json`: AEN
(`examples/peripheral-io/gpio-button-led`, m55_he + m55_hp) and V2N
(`examples/multicore/rpmsg-v2n`, m33_sm; the Yocto slice is skipped without
bitbake). The job fails on `ok:false`, on any expected Zephyr slice not `ok`,
or on a missing per-slice `zephyr.elf`, and uploads the envelope as an
artifact. NX91 has no Zephyr board in this repo, so it has no leg.
