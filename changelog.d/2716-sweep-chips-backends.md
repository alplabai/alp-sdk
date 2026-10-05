### Fixed — chip driver and backend sweep: RV-3028 alarm ack, DA9292 enable window, OPTIGA receive capacity, D/AVE 2D hang, ISP timestamp (#2716)

- **`rv3028c7_alarm_check_and_clear()`** now clears only the alarm flag; EVF, TF, UF, BSF and CLKF stay latched for `rv3028c7_dispatch_irq()`.
- **`da9292_set_enable()`** applies the enable-time window check before the already-enabled early return, so an out-of-window channel is refused even when already on.
- **`optiga_trust_m_send_apdu()`** sets the receive capacity after `session_open()`, so a drained timed-out op can no longer overwrite it.
- **D/AVE 2D** fill, blit and blend return `ALP_ERR_TIMEOUT` instead of hanging on a wedged engine.
- **ISP-PICO RGB565 capture** reads the frame timestamp before returning the raw buffer to the driver.
- **`camera-mjpeg-stream`** `CAMERA_MJPEG_STREAM_FPS` rejects 0 (division by zero).
