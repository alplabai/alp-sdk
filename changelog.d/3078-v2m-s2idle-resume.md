### Fixed — s2idle with an RTC wakealarm resumes on E1M-V2M / V2N (#3078)

**`echo freeze > /sys/power/state` (and a timed `alp_power_request_sleep()`) no longer panics the RZ/V2N board.** Three kernel patches in `meta-alp-sdk/recipes-kernel/linux/linux-renesas/`:

- `0033` (`rzv2h_wdt`, always applied): the watchdog core kept pinging the WDT after `rzv2h_wdt_suspend()` had asserted its reset, because systemd's `RuntimeWatchdogSec=30s` is above the ~175 ms hardware timeout and the core pings on its behalf; the ping raised an asynchronous SError in `watchdogd`. The driver now gates the ping under a spinlock from suspend until `start()` has the WDT counting again. `watchdog_stop_ping_on_suspend()` was tried and rejected: it stops pinging at `PM_SUSPEND_PREPARE` and the WDT bites during task freezing.
- `0034` (`drp`, with meta-rz-codecs / meta-rz-opencva) and `0035` (`drpai`, with meta-rz-drpai): `*_resume_noirq()` re-initialised a DRP / DRP-AI block that was never opened, whose CPG was still off, and the first register write raised an SError. Resume now re-initialises the hardware only when suspend found it running; the software state restore (status, IRQ-check flag, waitqueue wake) still runs every time.

Bench, E1M-V2M103 2026W38-0008, stock config (watchdog armed, drp-rz and drpai-rz bound), `rtc1` wakealarm +10 s: three s2idle cycles resumed (`suspend_stats` success 3, fail 0), then one cycle each with `/dev/drpai0` and `/dev/drp1` held open (DRP-AI re-initialised on resume, success 5, fail 0). The unpatched kernel panics on the first cycle.
