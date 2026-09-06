### Added — generate the V2N/V2M board `.dts` from metadata, closing #655

`scripts/gen_zephyr_board.py`'s `--emit zephyr-board` now generates the
E1M-V2N101 / E1M-V2M101 `m33_sm` board `.dts`, the last hand-authored file
in this family's board tree (#655 slice 2; slice 1, #1924, generated the
pinctrl.dtsi/_defconfig). `_v2n_dts()` reuses the upstream RZ/V2N SoC
devicetree and layers on the on-module GD32G553 supervisor links from
`metadata/e1m_modules/v2n/supervisor-links.yaml` -- the same source
`_v2n_pinctrl_dtsi()`/`_v2n_defconfig()` already read -- plus a new
`_v2n_part_display()` helper that derives the header's part string
(`"R9A09G056-N44"`) from the SoM preset's `silicon_variant` order code
rather than a hardcoded literal.

A new SoM preset field, `topology.m33_sm.openamp_ipc`
(`metadata/schemas/som-preset-v1.schema.json`), gates the OpenAMP/MHU-B
reserved-memory block and the CAN-FD-unavailable analysis comment that
only E1M-V2N101's committed tree carries today; it is unset for
E1M-V2M101, so the generator reproduces that asymmetry byte-for-byte
rather than inventing content for a board nobody has written it for yet.

Generated output for both boards is byte-identical to the previously
committed `.dts` apart from the standard `DO NOT EDIT BY HAND` banner --
the same shape as slice 1's pinctrl.dtsi/_defconfig delta, so this is
provably a no-op on hardware and needs no bench pass.
`tests/scripts/test_gen_zephyr_board.py`'s `test_v2n_full_tree_claimed`
replaces the old `test_v2n_dts_stays_hand_authored` ratchet (the `.dts`
is no longer the exception), and a new
`test_v2m_dts_reproduces_the_missing_openamp_block` proves the generator
can produce the shorter E1M-V2M101 tree, not just the longer one.
