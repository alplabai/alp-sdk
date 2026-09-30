### Fixed

- **V2N/V2M GD32 IO map** — E1M IO28 now maps to GD32 PE9 (was PC2). E1M IO24 is no longer listed as a GD32 pad (`dispatch: unrouted`; it is driven by the DX-M1 on V2M and undriven on V2N). E1M IO26 and IO15 are physically routed to GD32 PC2 and PB4 but have no bridge bit yet, so they are not exposed as bridge GPIOs. Bridge protocol and bit numbering are unchanged (#2453).
