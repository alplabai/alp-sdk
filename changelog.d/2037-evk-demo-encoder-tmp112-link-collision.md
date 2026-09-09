### Fixed — `aen-evk-demo` phase 7 (encoder) failed to link on a duplicate `tmp112_init` (#2037)

`CONFIG_SENSOR=y`, turned on only so `CONFIG_QDEC_ALIF` (the vendored UTIMER
quadrature decoder phase 7 needs) is reachable, also defaults Zephyr's
upstream TI TMP112 sensor driver
(`zephyr/drivers/sensor/ti/tmp112/tmp112.c`) to `y`. That collided at link
with alp-sdk's own `chips/tmp112/tmp112.c`, whose `tmp112_init` takes an
`alp_i2c_t*` and shares nothing but a name with upstream's `tmp112_init(const
struct device *dev)`:

```
ld.bfd: modules/alp-sdk/libalp_sdk.a(tmp112.c.obj): in function `tmp112_init':
  chips/tmp112/tmp112.c:53: multiple definition of `tmp112_init';
  zephyr/drivers/sensor/ti/tmp112/libdrivers__sensor__ti__tmp112.a(tmp112.c.obj):
  /home/caner/zephyr/drivers/sensor/ti/tmp112/tmp112.c:203: first defined here
```

`examples/aen/aen-evk-demo/prj.conf` now sets `CONFIG_TMP112=n`. This is
correct configuration, not a workaround: the demo already reads its TMP112
over alp-sdk's own chip driver (`CONFIG_ALP_SDK_CHIP_TMP112=y`, already set)
and has no use for the upstream sensor-subsystem driver for the same part.

**Architectural problem, recorded but not fixed here.** alp-sdk chip drivers
are named after the part by convention (`chips/tmp112/tmp112.c` →
`tmp112_init`); Zephyr's upstream sensor drivers for the same parts use the
same natural names for their own init functions. The two only collide when
both land in one image — which is exactly what `CONFIG_SENSOR=y` risks doing
for any alp-sdk chip driver whose part Zephyr also drives. This is the first
time it has bitten, but not the only part where it could: a search of
`zephyr/drivers/sensor` for every name under `chips/` in this repo also
turns up matches for `bme280`, `bmi323`, `bmp581`, `lis2dw12`, `lps22hb`,
`lsm6dso`, `hx711`, `max31855`, `max31865`, `veml7700`, `tsl2591`,
`vl53l1x` and `icm42670` — several of which (`bmi323`, `bmp581`, `icm42670`)
this very demo app already uses without incident, only because nothing in
its devicetree happens to auto-select those particular upstream Kconfig
symbols the way TMP112's got defaulted on. Any future board or app that
turns on `CONFIG_SENSOR` alongside one of these alp-sdk chip drivers can hit
the same wall. Fixing the naming convention itself (e.g. a symbol prefix for
alp-sdk chip drivers) is a separate, reviewed decision, out of scope here.
