### Fixed — CAST CAN `can_send()` accepts frames at the error-warning limit (#2983)

**`can_cast_send()` no longer returns `-EIO` once the error-warning limit is reached.** Error-warning is not bus-off, and an error-passive node still transmits, so the check is gone; only bus-off (`-ENETUNREACH`) and a stopped controller (`-ENETDOWN`) refuse a frame, as the Zephyr `can_send()` contract says. Covered by the new `tests/unit/can_cast_send_errwarn` test.
