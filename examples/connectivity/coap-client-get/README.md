# coap-client-get

`[UNTESTED]` -- native_sim build+run PASSED locally (see below), but this
has not been verified on real silicon (no bench/HIL sweep).

Teaches **libcoap** (<https://github.com/obgm/libcoap>), the full-featured
CoAP (RFC 7252) client/server library. `src/main.c` builds a CoAP GET
request PDU with `coap_pdu_init()` + `coap_add_token()` +
`coap_add_option()`, inspects it through libcoap's public getters, then
`coap_pdu_parse()`s a hand-encoded, RFC-7252-correct 2.05 Content response
and prints its payload. **No socket, no `coap_context_t`/`coap_session_t`**
-- this isolates the PDU codec half of libcoap (useful, deterministic,
runs on native_sim) from the transport half (needs a real UDP socket or a
DTLS session -- see "What's out of scope" below).

## What this shows

* `coap_pdu_init()` + `coap_add_token()` + `coap_add_option()` building up
  a request PDU's token/options in the PDU's own reserved buffer.
* Reading a PDU back through the PUBLIC API only: `coap_pdu_get_code()`,
  `coap_pdu_get_token()`, and a `coap_opt_iterator_t` walk over its
  options -- **not** direct `coap_pdu_t` struct field access (that struct
  is declared in `coap_pdu_internal.h`, explicitly `@ingroup internal_api`
  upstream; app code should never depend on its layout).
* `coap_pdu_parse()` turning a raw byte buffer (the same shape a
  `recvfrom()` would hand you) into a `coap_pdu_t`, then `coap_get_data()`
  reading the payload back out.
* The RFC 7252 CoAP/UDP wire format, hand-decoded byte-by-byte in the
  `canned_response[]` comment -- worth reading even if you never touch
  libcoap, as a primer on the header.

## What's out of scope (and why)

Serializing the *request* PDU to raw wire bytes (what you'd pass to
`sendto()`) requires `coap_pdu_encode_header()`, which is declared in
`coap_pdu_internal.h` (`@ingroup internal_api`) and, in the normal
send path, is called with a live `coap_session_t`'s protocol
(`coap_session_send_pdu()` in `coap_pdu.c`). Opening a real session means
a `coap_context_t` bound to a UDP socket, which native_sim can do but
needs either loopback traffic or a peer -- out of scope for a single
build-and-run example. This example demonstrates the PDU codec calls
(`coap_pdu_init`/`coap_add_token`/`coap_add_option`/`coap_pdu_parse`) that
are identical whether or not a session is involved; wiring the transport
is the next step once you have a real CoAP server to talk to.

## Library integration

libcoap is a **real, fetched module** at
`<west-workspace>/modules/lib/libcoap` (ships its own `zephyr/module.yml` +
`zephyr/CMakeLists.txt` + `zephyr/Kconfig`). Three gaps this example's
`CMakeLists.txt`/`prj.conf` route around, all documented inline:

1. `board.yaml`'s `cores.m55_hp.libraries: [libcoap]` only emits
   `CONFIG_ALP_COAP_NO_TLS=y` (the SW-fallback marker from
   `metadata/library-profiles/libcoap/hw-backends.yaml`) -- it does not
   reach into the fetched module's own `CONFIG_LIBCOAP`/
   `CONFIG_LIBCOAP_CLIENT_SUPPORT` symbols, so `prj.conf` sets those
   explicitly.
2. Upstream `CMakeLists.txt` turns on `-pedantic -Wall ... -Werror`
   unconditionally for the `coap-3` target -- flags written against
   libcoap's own headers, not Zephyr's (`BUILD_ASSERT`'s named variadic
   macro, zero-arg `__VA_ARGS__` uses, etc. fail under `-pedantic` the
   moment `coap_mem.c` pulls in `<zephyr/kernel.h>`). `CMakeLists.txt`
   here silences warnings on the already-defined `coap-3` target and
   restores its `zephyr_generated_headers`/`heap_constants_h` build-order
   dependency (missing because `coap-3` isn't registered through
   Zephyr's own `zephyr_library()` bookkeeping).
3. `coap_mem.c`'s Zephyr backend calls `k_malloc()`/`k_free()`, which need
   a real heap -- `prj.conf` sets `CONFIG_HEAP_MEM_POOL_SIZE=4096`
   (the hello-world skeleton default is `0`).

## board.yaml HW swap

`som.sku: E1M-AEN801` / `preset: e1m-evk`. This example does no I/O --
swap `som.sku` to any SoM/preset pair and it still builds; PDU
build/parse is entirely in-RAM.

## Build

```bash
west build -b native_sim/native/64 examples/connectivity/coap-client-get \
    -- -DEXTRA_ZEPHYR_MODULES="$(pwd);<west-workspace>/modules/lib/libcoap"
west build -t run
```

## Expected output

```
[coap-client-get] request: code=0x01 (GET), token=a1b2c3d4, Uri-Path="sensor"
[coap-client-get] response: 14 bytes, code=0x45 (2.05 Content)
[coap-client-get] payload: "23.5C"
[coap-client-get] done
```
