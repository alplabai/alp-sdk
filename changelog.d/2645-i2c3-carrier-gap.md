### Docs: X-EVK I2C table records the I2C3 carrier gap (#2645)

`docs/boards/e1m-x-evk.md` now notes that E1M-X I2C3 has no Linux master
except the GD32 I2C3 proxy (protocol 0.17), and that on the X-EVK V2 the J6
display I2C is not wired to I2C3 (seen on an E1M-V2M103 on 2026-10-06). The
`0x41` device on `i2c-0` is not the Riverdi touch controller. The GD32 bridge
at `0x70` on `i2c-8` was already listed.
