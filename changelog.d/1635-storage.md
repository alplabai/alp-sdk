### Fixed — a read-only storage handle answered `write()` and `erase()` with two different status codes for the same cause (#1635)

`src/storage_dispatch.c` split on `state.read_only`: `alp_storage_write()` returned
`ALP_ERR_NOT_READY`, `alp_storage_erase()` returned `ALP_ERR_INVAL` — same attribute, same
handle, same cause, two contracts. A read-only handle is a *state*, not a malformed argument:
the call is well-formed and would succeed on a writable handle. `alp_storage_erase()` now
returns `ALP_ERR_NOT_READY` too, matching `alp_storage_write()` and the convention the #1646
cluster (merged as PR #1713) settled for `alp_handle_op_enter()` failure — itself a state
problem, and already `ALP_ERR_NOT_READY` at 110 of 122 sites. ADR-0002 frames `ALP_ERR_INVAL`
as "programmer error" and a case could be made for that reading instead, but that rule is
already unimplemented at ~104 of 110 fused `op_enter` sites, so aligning storage with the real
convention beats aligning it with the aspirational one.

`include/alp/storage.h`'s `@return` docs for both `alp_storage_write()` and
`alp_storage_erase()` are updated to describe the unified behaviour — this was a documented
contract, not just an internal code path. A new `tests/unit/storage_registry` case
(`test_read_only_handle_write_and_erase_agree`) opens a read-only handle through the real
`alp_storage_open()`/`alp_storage_write()`/`alp_storage_erase()` public API and asserts both
calls surface `ALP_ERR_NOT_READY`, guarding against a regression back to `ALP_ERR_INVAL` on the
erase path.
