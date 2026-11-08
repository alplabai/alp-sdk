### Changed — wdt: alp_wdt_close() never disarms the watchdog on Yocto (#2981)

On Yocto, alp_wdt_close() no longer issues WDIOS_DISABLECARD or writes the magic close character, so the kernel watchdog keeps running after close, matching Zephyr. Code that relied on close() to stop the watchdog must now call alp_wdt_disable() first (or keep feeding it from another owner); a crash or exit after arming resets the board. wdt.h now states the single portable contract.
