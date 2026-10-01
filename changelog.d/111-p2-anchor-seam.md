### Added — update-log engine: optional external counter-anchor seam (#111, P2)

`ulog_engine_append_anchored()` / `ulog_engine_verify_anchored()` take an
optional `alp_counter_anchor_if` (`store.h`) holding a floor kept outside
the store. `verify` returns `ALP_UPDATE_LOG_VERIFY_ROLLED_BACK` when the
anchor is above the store's entry count, which is what a full reflash or tail
truncation looks like once a hardware anchor exists. The anchor advances only
after an append is fully committed, and every engine call first levels it up
to the committed count after torn-append recovery, so a crash at any point
never produces a false `ROLLED_BACK` and the floor lags by at most one entry
until the next call. `append` refuses (`ALP_ERR_IO`) while the floor is above
the store's count, so a rewound store cannot reuse sequence numbers and erase
the evidence; both callbacks are validated before anything is committed, and
an anchor read failure is returned as-is. With
no anchor (`NULL`, and the existing `ulog_engine_append`/`verify`) behaviour
is unchanged. Engine-internal: no public header change, no backend wires an
anchor yet (hardware anchor is P3).
