### Fixed — examples: aen-isp-capture fps divide, aen-power-stop alarm/regs/summary, cc3501e_bridge WIFI_EN latch (#2965, #2967, #2992)

aen-isp-capture divided by effective.numerator after checking only the
denominator; check the numerator.

aen-power-stop: disarm the stale RV-3028 alarm on every path that resets
the counter to 0, not only when it read 0 at boot; select the core AON
control register for the E4 M55-HP as well as the E8 HP; document where
the counter lives in each variant; mark SUMMARY scope=last_boot since
pass/fail restart at every cold-boot wake.

cc3501e_bridge: latch WIFI_EN high in DR also when the level read failed,
so the direction change cannot drop a running chip's supply.
