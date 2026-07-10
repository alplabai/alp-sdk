# jsmn-json-parse

`[UNTESTED]` -- native_sim build+run PASSED locally (see below), but this
has not been verified on real silicon (no bench/HIL sweep).

Teaches **jsmn** (<https://github.com/zserge/jsmn>), a minimal,
allocation-free JSON *tokenizer*. `jsmn_init` + `jsmn_parse` walk an
in-RAM JSON string once and fill a caller-owned `jsmntok_t[]` -- each
token is just `{type, start, end, size}` (byte offsets into the
*original* string, no copies, no heap). Turning tokens into typed C
values is on the caller; `src/main.c` shows the pattern (a string field
+ an int field) that covers most embedded config-blob parsing.

## What this shows

* `jsmn_init()` + `jsmn_parse()` against a fixed-size token array --
  worst-case RAM is `sizeof(jsmntok_t) * JSON_MAX_TOKENS`, known at
  compile time.
* Walking the flat token array as `[key, value]` pairs to extract a
  `device` string and an `interval_ms` int into a plain C struct.
* Checking `jsmn_parse()`'s return value (token count, or a negative
  `jsmnerr_t` on malformed/oversized input) before indexing the array.

## Library integration

jsmn is **vendored**, not west-fetched: `vendors/jsmn/include/jsmn.h`
is the unmodified upstream single header (tag `v1.1.0`, MIT). See
[`vendors/jsmn/README.md`](../../../vendors/jsmn/README.md) for why
(the `west.yml` `extras-tier1` pin is the audit trail, but this
worktree's west topdir hasn't fetched it). `CMakeLists.txt` adds
`vendors/jsmn/include` to the app's include path directly --
`board.yaml`'s `cores.m55_hp.libraries: [jsmn]` only carries the
`CONFIG_ALP_JSMN_SW=y` SW-fallback marker (jsmn has no HW-accelerator
class; see `metadata/library-profiles/jsmn/hw-backends.yaml`), not an
include-path hook.

## board.yaml HW swap

`som.sku: E1M-AEN801` / `preset: e1m-evk`. This example does no I/O --
swap `som.sku` to any SoM/preset pair (e.g. `E1M-V2N101` /
`e1m-x-evk`) and it still builds; jsmn parses an in-RAM string, no
peripheral or board-specific macro involved.

## Build

```bash
west build -b native_sim/native/64 examples/connectivity/jsmn-json-parse \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Expected output

```
[jsmn-json-parse] parsed 7 tokens
[jsmn-json-parse] device  = "e1m-aen801"
[jsmn-json-parse] interval_ms = 500
[jsmn-json-parse] done
```
