@page firmware_alp_stock_shim_index Alp stock shim firmware

# alp-stock-shim

Minimal Zephyr image for SoM preset M-core defaults that use
`app: alp-stock-shim`, and the CM33 image every E1M-V2M103 SoM ships with.

The shim claims no peripheral, no interrupt and no IPC endpoint: no `sci0`
(a floating RXD faults the core before `main`), no RIIC8 / `i2c-8`, no port 9
(GD32 SPI), no DMAC, no MHU. The board defaults enable the GD32 link for real
CM33 firmware, so `prj.conf` turns SPI and GPIO off and `app.overlay` disables
`&sci7` and `&gpio9`: P96 (SCK7) and P97 (chip-select) are left untouched. It gives the orchestrator a buildable, bootable
peer-core image when a project leaves a secondary M-core at the SoM default,
while customer applications can still override `cores.<id>.app` with their own
firmware.

## Liveness beacon

The CM33 has no console on these SoMs, so the shim proves it is running with
plain memory stores to the top of the `rsctbl` window (the board DTS's
`openamp_shm` reservation, CM33 view `0x9F700FF0`, which the A55 DT keeps
`no-map`). Layout and magic are the same as the `rpmsg-v2n` example:

| A55 address  | Word      | Value                                          |
|--------------|-----------|------------------------------------------------|
| `0x4F700FF0` | magic     | `0xA10D0683`, written last                     |
| `0x4F700FF4` | image kind/version | `0x00000100` (idle shim, no RPC) |
| `0x4F700FF8` | heartbeat | 0 at boot, then +1 about every second (`k_sleep`) |

Read it from Linux (`devmem` if the image has it, otherwise python3):

```sh
devmem 0x4F700FF0 32; devmem 0x4F700FF4 32; devmem 0x4F700FF8 32
python3 -c "import mmap,os,struct;m=mmap.mmap(os.open('/dev/mem',os.O_RDONLY|os.O_SYNC),4096,mmap.MAP_SHARED,mmap.PROT_READ,offset=0x4F700000);print([hex(x) for x in struct.unpack_from('<3I',m,0xFF0)])"
```

Magic `0xA10D0683` means "a CM33 image with an Alp beacon is running"; the word at `+0xFF4` says which: values below `0x100` are RPC firmware beacon versions (`1` today, `2` after #2586), `0x100` is this idle shim. If both match and the heartbeat grows between two reads, the CM33
is alive. Do not run this image together with an OpenAMP application: both
use the same beacon words.
