### Fixed — CC3501E GATT read/write wake on `alp_ble_disconnect()` (#3103)

**A `alp_ble_gatt_read()`/`alp_ble_gatt_write()` blocked on the CC3501E bridge now returns `ALP_ERR_NOT_READY` as soon as another thread calls `alp_ble_disconnect()`, instead of polling until its timeout.** The retry loop behind every bridge op (`poll_by_repeat_seq()`) gains a cancel flag checked before each attempt; `cc3501e_ble_gatt_read()` and `cc3501e_ble_gatt_write()` take a trailing `const volatile bool *cancel` (pass `NULL` for none) and the BLE backend points it at the connection's closing flag.
