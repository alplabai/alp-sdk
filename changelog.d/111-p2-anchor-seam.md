### Added — update-log engine: optional external counter-anchor seam (#111, P2)

`ulog_engine_append_anchored()` / `ulog_engine_verify_anchored()` take an
optional `alp_counter_anchor_if` (`store.h`) holding a floor kept outside
the store. `verify` returns `ALP_UPDATE_LOG_VERIFY_ROLLED_BACK` when the
anchor is above the store's entry count, which is what a full reflash or tail
truncation looks like once a hardware anchor exists. The anchor advances only
after an append is fully committed and is compared only after torn-append
recovery, so a crash at any point never produces a false `ROLLED_BACK`. With
no anchor (`NULL`, and the existing `ulog_engine_append`/`verify`) behaviour
is unchanged. Engine-internal: no public header change, no backend wires an
anchor yet (hardware anchor is P3).
