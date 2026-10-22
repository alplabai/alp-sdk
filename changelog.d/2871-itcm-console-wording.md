### Fixed — `diagnostics.link: itcm` console wording and the generated PM wake-source hint (#2871, #2889)

The `board.schema.json` description of `diagnostics.link` now states the planner's actual rule: `console: auto` is promoted to `ram` and `ram` use the RAM console, an explicit `uart` (alias `alp`) RAM-runs with the board UART console and gets neither `CONFIG_DCACHE=n` nor the RAM-console buffer, and `linux`/`none` are refused. The `# wakeup source:` hint written into the generated `alp.conf` now says `pm_device_wakeup_enable() not implemented` instead of promising it for v0.7.
