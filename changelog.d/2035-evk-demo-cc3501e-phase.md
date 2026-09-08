### Added — `aen-evk-demo` phase 8 exercises the CC3501E Wi-Fi 6 / BLE 5.4 coprocessor for real (#2035)

Phase 8 was a `SKIPPED` stub. It now powers, resets and drives the on-module
TI CC3501E over the SoM-internal inter-chip SPI1 bridge, so eight of the
demo's fourteen phases are implemented and six remain stubs.

**The supply is host-gated, so the phase starts by turning the part on.** The
CC3501E has no power until the Alif drives `WIFI_EN` (P15_5) high — a J-Link
cannot even attach to it before that. The phase runs
`cc3501e_bridge_bringup()`, the SoM bring-up template copied byte-for-byte
from `examples/aen/aen-cc3501e-bringup`: it opens SPI1 (P14_6 SCK / P14_5
MOSI / P14_4 MISO with the dwc-ssi **hardware SS0** on P14_7, `ALP_SPI_NO_CS`
so the controller frames each protocol phase itself), enables the LP-GPIO pad
output drivers that pinctrl does not reach, binds the `READY` input (P2_6 <-
CC35 `GPIO_17`) so each reply phase is gated on the slave's actual re-arm,
and runs the power + reset sequence including the Puya-flash double-boot
workaround. The app's overlay gains the matching wiring; the board layer does
not publish these nets because they are SoM-internal, not E1M edge pads.

It then issues, printing every return code verbatim: `PING` (`0x00`) on a
bounded 25 x 200 ms retry, `GET_VERSION` (`0x01`), `GET_MAC` (`0x03`),
`GET_CAPABILITIES` (`0x06`), a passive `WIFI_SCAN_START` (`0x10`), and
`BLE_ENABLE` (`0x30`).

**A `PING` answering is not a `PASS`.** That is the same shape of claim as
the chip-ID read this whole app exists to stop counting — it proves the
transport and nothing about either radio. `PASS` additionally requires all
five of:

* the reported protocol version **equals** `ALP_CC3501E_PROTOCOL_VERSION` —
  `cc3501e_get_version()` deliberately does not compare (its callers include
  liveness soaks), and a version that round-trips while disagreeing means
  every later reply is being parsed against the wrong layout;
* `GET_MAC` returning a **structurally valid station address** — all-zero,
  broadcast `ff:ff:ff:ff:ff:ff`, or the IEEE group bit set in the first octet
  are each rejected as "the radio has not read its identity out", the
  MAC-shaped equivalent of a reset sentinel. A TI-block address is *not*
  rejected: the CC3501E carries a TI factory MAC (MA-L `44:3e:8a` on bench
  units), Alp Lab holds no IEEE OUI and needs none, and rejecting one would
  fail every correctly provisioned module;
* `GET_CAPABILITIES` answering, so the log states what the firmware
  implements rather than inferring it from a version number;
* the Wi-Fi scan **round-tripping** (see below);
* `BLE_ENABLE` bringing the controller + NimBLE host up. `ALP_ERR_NOT_READY`
  there means BLE is not built into that firmware, which is a real deviation
  from what every shipped SoM carries, so it fails rather than skips.

Three of those five (`GET_MAC`, the scan, `BLE_ENABLE`) are worker-routed on
the firmware side rather than answered from its SPI ISR, so they cannot be
satisfied by a coprocessor merely running its dispatch loop with dead radios.

**An empty scan is `PASS`-but-`UNCORROBORATED`, and the log says so in
words.** Zero networks is a statement about the RF environment — a shielded
room or a quiet band are both real — not about this board, so the gate is
that the scan completed and its reply parsed, not that it found anything. It
is a materially stronger claim than a register read: `WIFI_SCAN_START` is
poll-by-repeat, so `ALP_OK` means the request was accepted, the scan worker
ran to completion, and a well-formed payload came back. When records *do*
arrive they are checked rather than tallied — a record needs a non-zero BSSID
and a genuinely negative, above-noise-floor RSSI — so a reply full of zeroed
records fails instead of reporting "networks seen".

A part that does not power up or does not answer is a `FAIL` with its return
codes and the three things to check (WIFI_EN, the SPI1 pinmux, the READY
line), never a silent skip: the part is fitted on every E1M-AEN SoM and this
phase is what powers it.

**The phase never joins a network and never touches firmware.**
`cc3501e_wifi_connect()` is not called and no credentials exist in the app or
its build — a scan is passive listening; associating would put a bench board
on somebody's network. There is likewise no activation, provisioning or
firmware-flash path, and none may be added: the parts ship already activated,
no SDK opcode can activate one, and the fuses involved are OR-only, so a
botched attempt permanently bricks a unit's secure boot. The driver's
`cc3501e_ota_*` opcodes are deliberately never called here.

The phase leaves `WIFI_EN` high, the handle bound and BLE enabled on purpose:
the SD-card phase's SDIO mux (`EN`/`SEL` on CC35 `GPIO_26`/`GPIO_30`) is
reachable only through this coprocessor, so powering it back down would make
that phase impossible to add later. Phase 9's stub is re-worded accordingly —
it is no longer blocked on phase 8, only on the SD side itself.

Builds clean for `alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` at
`FLASH: 143296 B / 256 KB (54.66%)` — up from `108556 B (41.41%)`, the
CC3501E bridge driver accounting for ~34 KB — and `RAM: 60504 B (23.08%)`,
which is mostly the ~32 KB `cc3501e_t`. That handle is file-static rather
than a stack local for two reasons: as a local it crosses `PSPLIM` and the
M55 raises `STKOF` -> UsageFault before a line is printed, and the SD-card
phase will need the same bound handle. Still inside the Flow C ITCM budget,
with the NPU stub's headroom note updated to the new figure. The phase's own
`PASS`/`FAIL` needs a bench run on `e1m-aen-evk-03`; the seven previously
implemented phases are unchanged.
