### Added — windowed watchdog feed and sleep/debug pause in `<alp/wdt.h>` (#1637)

`alp_wdt_config_t` gains two fields, both zero by default, so existing configs are unchanged:

- **`window_min_ms`.** A feed earlier than this many milliseconds after the previous one counts as a violation. It must be below `timeout_ms`.
- **`flags`.** `ALP_WDT_PAUSE_IN_SLEEP` and `ALP_WDT_PAUSE_HALTED_BY_DEBUG`.

On Zephyr they map to `wdt_timeout_cfg.window.min` and to the `WDT_OPT_PAUSE_IN_SLEEP` / `WDT_OPT_PAUSE_HALTED_BY_DBG` options of `wdt_setup()`. A driver that cannot do a window or a pause makes `alp_wdt_open()` fail with `ALP_ERR_NOSUPPORT`; the request is never silently dropped. The Linux `/dev/watchdog` backend has no such controls, and the software fallback has no timer, so both refuse a non-zero value the same way.

The dispatcher rejects `window_min_ms >= timeout_ms` and unknown `flags` bits with `ALP_ERR_INVAL`, before any backend runs. This completes #1637. The close-disarms-the-whole-device defect and `ALP_WDT_INTERRUPT_ONLY` landed earlier in #1928.
