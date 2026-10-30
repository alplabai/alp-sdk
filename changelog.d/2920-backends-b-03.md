### Fixed — ble,pwm: clamp CC3501E scan adv_len, copy GATT write payload, PWM open/period/polarity contract (#2920, #2927, #2928, #2929, #2979)

ble/cc3501e: scan results reported the raw wire name_len (up to 255) as
adv_len over a 32-byte name buffer; clamp to CC3501E_BLE_NAME_MAX.

ble/zephyr_drv: gatt_write handed the caller's buffer to the host, which
keeps reading it after a timeout. Copy the payload (max 512 octets, the GATT
attribute limit; larger returns ALP_ERR_INVAL instead of truncating to
uint16) into the pool-owned ctx.

pwm/gd32_bridge: set_period now sends 0 duty as <alp/pwm.h> requires; open
programs the channel at 0 % duty and returns ALP_ERR_NOSUPPORT for INVERTED
polarity (the bridge has no polarity field).

pwm/yocto_drv: a rejected polarity write now fails open (a NORMAL request
is tolerated only if the channel reads back "normal").

pwm/zephyr_drv: start from the devicetree spec flags and XOR the requested
inversion on top, so a DT PWM_POLARITY_INVERTED channel keeps its polarity.
