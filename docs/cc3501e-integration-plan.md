# CC3501E Integration Plan (SWRU626 Deep-Dive)

> **Superseded (2026-07-15).** This was the May-2026 pre-implementation
> plan for the CC3501E Wi-Fi/BLE bridge, written before host or firmware
> code existed. The bridge has since **shipped** (2026-07-05,
> silicon-proven: Wi-Fi + BLE + sockets + diagnostics + OTA) — in places
> **differently** from what this plan proposed. Two notable examples: the
> plan proposed `ALP_CC3501E_CMD_POWER_POLICY` at opcode `0x04`; it shipped
> at `0x62`. The plan proposed a polled `busy_pin` GPIO handshake; the
> shipped design uses a hardware-framed SPI chip-select (SS0) with a
> per-phase READY handshake instead. Do not use this document as a
> reference for current behaviour.
>
> The still-relevant survivors (peripherals not proxied, and the two
> genuinely open questions) were merged into
> [`cc3501e-bridge.md`](cc3501e-bridge.md). Everything else here is
> either implemented, answered, or superseded by a more accurate doc.
>
> For current information, see:
>
> - [`cc3501e-bridge.md`](cc3501e-bridge.md) — bridge architecture,
>   transport, boot model, GPIO contract, OTA, versioning.
> - [`cc3501e-companion-commands.md`](cc3501e-companion-commands.md) —
>   `alp companion` diagnostic console.
> - [`cc3501e-gpio-bench.md`](cc3501e-gpio-bench.md) — GPIO-proxy bench
>   validation.
> - [`cc3501e-production.md`](cc3501e-production.md) — building,
>   signing, and provisioning a shippable CC3501E image.
