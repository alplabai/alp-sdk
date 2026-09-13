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

Backward compatibility is the load-bearing part, and it works by accident of
the wire's own padding, not a length check: the firmware zero-pads every
reply to an 8-byte multiple and folds pad + CRC into the declared
`payload_len`, so an 18-byte-firmware reply and a pre-#2035 16-byte-firmware
reply both frame the identical 24-byte wire payload -- `cc3501e_diag_info()`
sees the same byte count either way and simply reads `dhcp_state`/
`netif_status` directly; against old firmware those two bytes land on the
firmware's zero pad ahead of the CRC trailer, which is exactly the
"not reported" encoding. A reply shorter than the original 16 bytes still
fails. `chips/cc3501e/cc3501e_diag.c`'s comment and
`tests/zephyr/cc3501e_host_driver/src/test_host_driver.c` now spell this out,
with a second backward-compat test staged through the genuinely-unpadded
legacy wire shape (the one case that actually falls below the guard) so the
property is pinned by mutation, not just by a passing assertion.

`diag info` (both the `alp companion` console command and the
`aen-cc3501e-companion-tour` example) now prints the decoded DHCP state name
and netif up/link/retry-count line; `docs/cc3501e-companion-commands.md`'s
table lists it.

This is deliberately NOT a `PROTOCOL_MINOR` bump. A host disambiguates
"talking to old firmware" from "DHCP never started" -- both read
`dhcp_state` as 0 -- by the reply's own `fw_version` field, not by the new
fields' value, so no version check is needed to use them safely. A MINOR
bump would still force a lockstep change in the firmware's
`protocol-version.txt`, but since that job checks out alp-sdk's *default*
branch, a bump on `dev` would not redden firmware PRs until the next
`dev`-to-`main` release, not immediately.
