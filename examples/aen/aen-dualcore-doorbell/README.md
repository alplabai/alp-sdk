<!--
Copyright (c) 2026 Alp Lab AB
SPDX-License-Identifier: Apache-2.0
-->

# aen-dualcore-doorbell — HE→HP MHU-1 doorbell with both M55 cores live

The completion of **B1**. Earlier doorbell attempts could not be tested because
only one M55 ran (a dual-boot ATOC boots one core) and a J-Link debug-AP write to
the sender did not propagate. Now `aen-dualcore-master` brings both cores up
(`se_service_boot_cpu`), so a **real HE-core sender** can ring HP.

The HP build is the master + receiver (boots HE, then polls the MHU-1 receiver);
the HE build is the sender (rings the MHU-1 sender). MHU-1 is the **non-secure
HE→HP** pair (Alif DFP + fork `e1.dtsi`):

| | base | IRQ | role |
| --- | --- | --- | --- |
| sender (HE writes) | `0x400B0000` | 44 | `CH0_SET` +0x0C; `ACCESS_REQUEST` +0xF88; `ACCESS_READY` +0xF8C |
| receiver (HP reads) | `0x400A0000` | 43 | `CH0_ST` +0x00; `CH0_CLR` +0x08 |

(register offsets transcribed from `zephyr/drivers/ipm/ipm_arm_mhuv2.h`.)

## Result (bench-verified on E8, 2026-06-18) — every ring received ✅

```
HP (master+receiver) : magic B1B10090  hb 0x823 -> 0x94F   received 0x01F4 -> 0x023C
HE (sender)          : magic B1B100E0  hb advancing         sent     0x01F4 -> 0x023C
```

**HE sent count == HP received count, exactly** — every doorbell HE rings is
received by HP. The HE→HP MHU-1 doorbell propagates with both cores live, with
**no SESS / secure-MHU setup needed** (the non-secure MHU-1 pair works directly).
This is the working substrate for HE↔HP IPC / a dual-core RPC.

> The earlier "J-Link-as-sender does not propagate" finding was a debug-AP
> artifact, not a hardware limit — a real CPU write to `CH0_SET` propagates.

Recipe: dual ATOC with HP-APP `["load","boot"]` @0x50000000 + HE-APP `["load"]`
@0x58000000; `app-gen-toc` + `app-write-mram`. Restore the canonical slot0 after.
