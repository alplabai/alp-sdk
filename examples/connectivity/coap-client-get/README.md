# coap-client-get

`[UNTESTED]` -- native_sim build+run PASSED locally (see below), but this
has not been verified on real silicon (no bench/HIL sweep).

Teaches **CoAP** (RFC 7252, <https://www.rfc-editor.org/rfc/rfc7252>), the
constrained-device REST transport this SDK ships as the **in-tree Zephyr
subsystem** `subsys/net/lib/coap/` (see `metadata/libraries/coap.yaml`).
`src/main.c` builds a CoAP GET request PDU with `coap_packet_init()` +
`coap_packet_append_option()`, inspects it through the public getters, then
`coap_packet_parse()`s a hand-encoded, RFC-7252-correct 2.05 Content response
and prints its payload. **No socket, no `net_context`, no `coap_client`** --
this isolates the PDU codec half of CoAP (useful, deterministic, runs on
native_sim) from the transport half (needs a real UDP socket -- see "What's
out of scope" below).

## What this shows

* `coap_packet_init()` + `coap_packet_append_option()` building up a request
  PDU's header/token/options directly into a caller-owned byte buffer (CoAP's
  Zephyr API has no separate "PDU object" vs "wire buffer" -- they're the
  same buffer from the start).
* Reading a PDU back through the PUBLIC API only: `coap_header_get_code()`,
  `coap_header_get_token()`, and `coap_find_options()` -- **not** direct
  `struct coap_packet` field access (the header documents those fields as
  "CoAP lib maintains", i.e. internal bookkeeping app code should not read).
* `coap_packet_parse()` turning a raw byte buffer (the same shape a
  `recvfrom()` would hand you) into a `struct coap_packet`, then
  `coap_packet_get_payload()` reading the payload back out.
* The RFC 7252 CoAP/UDP wire format, hand-decoded byte-by-byte in the
  `canned_response[]` comment -- worth reading even if you never touch CoAP
  directly, as a primer on the header.

## What's out of scope (and why)

Sending the *request* PDU over the wire needs a bound UDP socket
(`zsock_sendto()`/`zsock_recvfrom()`) or Zephyr's higher-level
`CONFIG_COAP_CLIENT` (an `[EXPERIMENTAL]` layer over `net sockets` --
`subsys/net/lib/coap/coap_client.c`). Opening a real socket means live
loopback traffic or a peer, which native_sim can do but is out of scope for
a single build-and-run example. This example demonstrates the PDU codec
calls (`coap_packet_init`/`coap_packet_append_option`/`coap_packet_parse`)
that are identical whether or not a socket is involved; wiring the
transport is the next step once you have a real CoAP server to talk to.

## Library integration

CoAP is the **in-tree Zephyr subsystem** at `$ZEPHYR_BASE/subsys/net/lib/coap/`
-- unlike a fetched west module, it ships inside the `zephyr` project itself,
so there is nothing to pin in `west.yml`. `board.yaml`'s top-level
`libraries: [coap]` (ADR 0018 -- see `metadata/libraries/coap.yaml`) makes
`alp_project.py` emit two Kconfig lines into the generated `alp.conf`:

1. `CONFIG_NETWORKING=y` -- `CONFIG_COAP` lives `if NETWORKING`
   (`subsys/net/lib/Kconfig` is sourced inside `subsys/net/Kconfig`'s
   `if NETWORKING` block), so this is a hard prerequisite, not optional.
2. `CONFIG_COAP=y` -- enables `subsys/net/lib/coap/coap.c`, the packet codec
   this example calls. `CONFIG_COAP_CLIENT` (the socket-based client layer)
   is deliberately **not** enabled -- this example never opens a socket, so
   it only needs the low-level packet API.

`prj.conf` carries only the app-specific `CONFIG_MAIN_STACK_SIZE` -- no heap
knob is needed (unlike some CoAP implementations, the in-tree codec works
entirely off caller-owned stack/static buffers, no `k_malloc()`).

## board.yaml HW swap

`som.sku: E1M-AEN801` / `preset: e1m-evk`. This example does no I/O --
swap `som.sku` to any SoM/preset pair and it still builds; PDU
build/parse is entirely in-RAM.

## Build

```bash
west build -b native_sim/native/64 examples/connectivity/coap-client-get
west build -t run
```

## Expected output

```
[coap-client-get] request: code=0x01 (GET), token=a1b2c3d4, Uri-Path="sensor"
[coap-client-get] response: 14 bytes, code=0x45 (2.05 Content)
[coap-client-get] payload: "23.5C"
[coap-client-get] done
```
