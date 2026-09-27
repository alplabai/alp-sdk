### Fixed — E1M-X EVK TCAL9538 addresses (#2333)

The E1M-X EVK carries two TCAL9538 I/O expanders on its sensor bus (`XEVK_I2C_BUS_SENSORS` / E1M_X_I2C0), but `metadata/boards/e1m-x-evk.yaml` recorded a single one at `0x72`. The board strapping, confirmed against the E1M-X EVK V2 netlist and a live `i2c-0` sweep on an E1M-V2M103, is:

- U35 main expander at `0x73` (A0 and A1 high)
- U37 PCIe expander at `0x71` (A0 high, A1 low). It sits on `PCIE0_I2C`, which is joined to I2C0 through an LSF0102 level shifter.

The single `XEVK_I2C_ADDR_TCAL9538` macro is replaced by `XEVK_I2C_ADDR_TCAL9538_MAIN` (`0x73`) and `XEVK_I2C_ADDR_TCAL9538_PCIE` (`0x71`). `XEVK_I2C_ADDR_TCAL9538` stays as an alias for the main expander. `metadata/chips/tcal9538.yaml`'s address scopes and the v0.16 ABI snapshot are updated to match.

On Linux, the sensors on this bus are driven by the SDK's own `chips/` drivers over i2c-dev, the same drivers the AEN EVK uses, not by kernel drivers. Verified on the E1M-V2M103: `examples/v2n/v2n-power-monitor` (`chips/ina236`) reads 3V3 = 3.302 V and 1V8 = 1.792 V. Two carrier defects found while checking the netlist are tracked in #2343: U30 is an INA228 with SDA/SCL swapped, and the two IMUs are both strapped to `0x69`. The U32 VCAM2 monitor's `0x48` also collides with the TAS2563 broadcast address on this bus; U32 is removed on the current build batch and its metadata entry now says so.
