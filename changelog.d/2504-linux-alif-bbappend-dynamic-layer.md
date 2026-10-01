### Fixed — a V2N/V2M Yocto build no longer halts at parse on a dangling `linux-alif` bbappend (#2504)

The E1M-AEN console-routing bbappend added for #1979 sat in the plain
`recipes-kernel/` tree, where BitBake requires a matching recipe. `linux-alif`
exists only in `meta-alif-ensemble`, so any other MACHINE (for example
`e1m-v2m103-a55`) failed with a dangling-bbappend error unless
`BB_DANGLINGAPPENDS_WARNONLY=1` was set.

It now lives at
`meta-alp-sdk/dynamic-layers/meta-alif-ensemble/recipes-kernel/linux/linux-alif_%.bbappend`
and is registered in `BBFILES_DYNAMIC` at
`meta-alp-sdk/conf/layer.conf:34`
("meta-alif-ensemble:${LAYERDIR}/dynamic-layers/meta-alif-ensemble/*/*/*.bbappend"),
the same way `meta-deepx-m1` is, so it is parsed only when that collection is in
`bblayers.conf`. The `e1m-aen-evk-console.dtsi` fragment moved alongside it.
The collection name `meta-alif-ensemble` matches the existing
`LAYERRECOMMENDS_alp-sdk` entry; confirm it against the vendor layer's own
`BBFILE_COLLECTIONS` once a Scarthgap branch exists (#1968).
