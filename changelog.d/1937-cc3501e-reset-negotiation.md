### Fixed — `cc3501e_reset()` could return `ALP_OK` without ever negotiating the wire MAJOR (#1937, #1927)

Found on E1M-AEN803 silicon (1 of 5 matched cold boots): the reset-time `GET_VERSION` missed, the transport-miss branch of `cc3501e_reset()` (`chips/cc3501e/cc3501e_core.c`) restored the "previous" MAJOR — `0` on a fresh context — and returned `ALP_OK` with `initialised=1`. Every later request was framed CRC-less against a MAJOR-4 firmware, so `GET_CAPABILITIES` and every `PING` failed `-5` for the whole run while a bare `GET_VERSION` kept answering.

`cc3501e_reset()` now retries the version query (3 attempts, 200 ms apart) and, if a fresh context still negotiates nothing, returns `ALP_ERR_TIMEOUT` with `initialised=0`, so later calls fail `ALP_ERR_NOT_READY` instead of `-5`. A context that already negotiated a MAJOR keeps it, as before. `aen-cc3501e-bringup` now compares only the MAJOR when printing the `GET_VERSION` verdict (MINOR skew is compatible, ADR 0033).
