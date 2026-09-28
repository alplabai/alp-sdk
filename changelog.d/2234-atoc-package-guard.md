### Fixed — Flow A/D bench scripts refuse an ATOC package that grows into customer `storage` (#2234)

The metadata reserves 32 KiB for the ATOC (`atoc` band at `0x80578000`),
sized for a TOC-only package. SETOOLS stores an ITCM load image's bytes
(`loadAddress`, no `mramAddress`) inside the top-anchored package, so a
Flow A package grows downward past that band into `storage`. On an AEN803
the measured package was 89152 B, starting at `0x8056A3C0`. A customer
runtime writing `storage` would then overwrite the boot image, and so
would `erase-storage.sh`.

A new `bench_atoc_package_fits` in `scripts/bench/aen/bench-env.sh` reads
the package start from SETOOLS' `app-package-map.txt` and the `atoc` base
from the AEN801/AEN803 metadata. It refuses (exit 6) when the package
starts below that base, unless `BENCH_ALLOW_ATOC_INTO_STORAGE=1` is set.
All seven scripts that run `app-gen-toc` call it before writing
(`flash-run.sh`, `flash-run-dualcore.sh`, `flash-jlink.sh`,
`flash-jlink-hp.sh`, `flash-jlink-mramxip.sh`,
`flash-update-log-dual.sh`, `flash-update-log-firewall-probe.sh`).

This is option 3 of #2234: the flows refuse the layout. Resizing the
band, or keeping load images out of the package, is still open.

Verified against real SETOOLS `app-gen-toc` output, generated offline
with nothing written to MRAM. The staged `aen-npu-inference-person-mram`
slot0 package (start `0x8057ea50`) passes. A 146 KB ITCM load-image
package (start `0x8055aff0`, 118800 B below the band) is refused.
