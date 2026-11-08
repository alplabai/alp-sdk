### Fixed — dispatch,model: self-close marker ownership, ble open publish order, model arena/blob validation (#2919, #2977)

- alp_slot_claim: alp_handle_cb_enter() now claims the self-close marker
  with a CAS and returns whether the caller owns it; alp_handle_cb_leave()
  only clears an owned marker. A second overlapping alp_ble_scan_start()
  or alp_mqtt_loop() on the same handle no longer overwrites cb_thread or
  clears cb_active, so the first thread's real self-close is still
  recognised instead of deadlocking on its own op count.
- ble_dispatch: alp_ble_open() publishes lifecycle OPEN before refcount,
  so a joiner that sees refcount > 0 always sees an OPEN handle; stray
  closes can no longer leave the slot in_use with refcount 0 and spin
  later opens forever.
- camera_dispatch: alp_camera_capture(NULL, ...) returns ALP_ERR_INVAL as
  documented in <alp/camera.h>.
- alp_model: a target map without a "blob" key is rejected instead of
  silently bound to blob 0.
- alp_model_loader: a caller-owned arena with arena_bytes == 0 is
  rejected with ALP_ERR_INVAL; the manifest never sizes a caller buffer.
