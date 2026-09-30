### Fixed - `v2n-gd32-bridge-loopback` README named the wrong header pin for jumper B (#2462)

Jumper B runs from CK_PWM1, which is `J26.7` on the E1M-X EVK carrier, not `J26.14`. The other rows already matched the carrier pinmap (CK_PWM2 = `J26.10`, CK_PWM3 = `J26.8`).
