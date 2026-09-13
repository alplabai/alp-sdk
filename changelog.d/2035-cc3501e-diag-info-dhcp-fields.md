### Added — host driver decodes the CC3501E bridge's two new GET_DIAG_INFO bytes: lwIP DHCP state and netif flags/tries (#2035)

`cc3501e-bridge-firmware`'s `src/protocol_diag.c` grew the `GET_DIAG_INFO`
(opcode `0x04`) reply from 16 to 18 bytes on `main`: byte 16 is lwIP
`dhcp->state` PLUS ONE (0 = "not reported" -- no lwIP linked, or the netif
has no `dhcp` struct because DHCP never started, itself the interesting
case that must not be misread as a real `DHCP_STATE_OFF`); byte 17 packs
bit0 = netif UP, bit1 = netif LINK_UP, bits 2..7 = `dhcp->tries` saturated
at 63. `chips/cc3501e/cc3501e_diag.c` still passed a 16-byte buffer, so
`cc3501e_request()` silently truncated both bytes and no host could see
them -- two bench sessions had to read them out of `ctx->rx_scratch` at a
hardware breakpoint instead, and one mis-indexed it (`rx_scratch[0]` is the
RESP status byte, so payload byte 16 is `rx_scratch[17]`), reporting the
fields as stale scratch rather than data.

`include/alp/protocol/cc3501e.h` adds `dhcp_state` and `netif_status` to
`alp_cc3501e_diag_info_t`, plus `alp_cc3501e_dhcp_state_t`
(`ALP_CC3501E_DHCP_STATE_NOT_REPORTED`/`_OFF`/`_SELECTING`/`_BOUND`, named
distinctly from the unrelated `ALP_CC3501E_RADIO_EVT_*` family and mirroring
lwIP's `prot/dhcp.h`) and the `ALP_CC3501E_NETIF_UP` /
`ALP_CC3501E_NETIF_LINK_UP` / `ALP_CC3501E_NETIF_DHCP_TRIES()` bit helpers.

Backward compatibility is the load-bearing part: `cc3501e_diag_info()` now
reads up to 18 bytes but only *requires* 16 -- an older bridge that still
answers 16 bytes is a SUCCESS with both new fields reported as `0`
("not reported"), not an `ALP_ERR_IO` regression. A reply shorter than the
original 16 bytes still fails.

This is deliberately NOT a `PROTOCOL_MINOR` bump -- the encoding is
self-describing per field, and a bump would force a lockstep change in the
firmware's `protocol-version.txt` that would redden every firmware PR until
both sides landed.
