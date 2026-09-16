### Added — EEPROM Secure Data Page mirror + write/lock API (#2151)

A new 64-byte `alp_secure_page_mirror_t` (`<alp/hw_info.h>`) and two new
driver calls, `eeprom_24c128_secure_page_write()` /
`eeprom_24c128_secure_page_lock()` (`<alp/chips/eeprom_24c128.h>`), give
alp-sdk a write + permanent-lock path for the onsemi N24S128's Secure Data
Page (`0x58`, selector `0x00`, 64 bytes) — a recovery copy of the array
manifest's immutable core (`sku`, `hw_rev`, `serial`, mfg date, CRC), since
the array manifest at `0x50` offset `0x0000` has no write protection at all
and one stray `eeprom_24c128_write(ctx, 0, ...)` from customer code can wipe
it. `_Static_assert`-pinned at exactly 64 bytes, magic `0x414C5350` ("ALSP"),
`schema_version` independent of the array manifest's. Full byte table + the
width reasoning: `EEPROM-MANIFEST-SPEC.md`'s Secure Data Page section
(alp-sdk-internal).

Deliberately NOT a straight truncation of the 128-byte manifest's field
widths — those sum to exactly 64 with no room left for a magic or a schema
version, which would make a blank (unprovisioned, all-`0xFF`) page
indistinguishable from a provisioned one whose serial happens to start with
`0xFF`, and would freeze every locked unit at whatever version shipped
first. The new struct's field widths (`sku[16]`, `hw_rev[12]`, `serial[23]`)
are sized from what the data needs today (10/7/12 chars respectively) plus
headroom sized to how likely each field is to outgrow it, not copied from
the manifest's and not split evenly: `sku` gets the most headroom (1.6x)
because it is both the field the array itself budgets the most headroom for
(24 bytes against the same 10-char pattern) and the one this mirror exists
to recover — a future SKU the array could still hold but this locked mirror
could not represent would defeat the mirror's purpose. `hw_rev` gets the
least (1.7x, but off a smaller base) because it tracks a physical Altium
board revision, the field least likely to ever need more room. A reader
that encounters a `schema_version` it has no code for — lower or higher —
must refuse to parse the struct rather than guess, the same rule the array
manifest's reader already applies, stated explicitly in the spec because a
locked page's reader bugs are permanent in a way the rewritable array's
are not.

Two separate functions, not one write-and-lock call, so the lock is
gateable on a caller-verified read-back — the lock is one-shot and
permanent (`docs/som-batch-provisioning-procedure.md` §7,
alp-sdk-internal): after it, "Any write instructions to the Secure Data
Page will return No ACK from the device." `eeprom_24c128_secure_page_write()`
takes no offset/selector parameter — it always writes the full page from a
compile-time-fixed selector — so it is structurally impossible for it to
ever construct a write to selector `0x06` (the Device Configuration
Register), where a stray byte would land on the `SWP` bit and permanently
write-protect the array, this page, and the register together.
`eeprom_24c128_secure_page_lock()` re-reads Secure Page Lock Status after
issuing the lock, rather than trusting the write's own ACK, and neither
function implements the datasheet's other lock-check method (attempt a
page write and see whether it ACKs) since that method is itself a write.
Both functions are [UNTESTED] / [PAPER-ONLY] against silicon — their exact
wire bytes are transcribed from the provisioning procedure doc, the only
source available, and need a real bench run before the first production
lock.

`examples/aen/aen-eeprom-provision` gains two new build-time modes
alongside its existing array-write default: `-DALP_SECURE_PAGE_BIN=<path>`
writes the mirror and verifies the read-back in the same run (never
locks); `-DALP_LOCK_SECURE_PAGE=1` (which requires
`-DALP_SECURE_PAGE_BIN` too) permanently locks it, refusing unless the
page currently on the device — read fresh, since a RAM-run cannot survive
a power cycle — is byte-exact against the same blob the write mode used.
The lock is never a side effect of writing; it needs its own explicit
opt-in flag, the same shape as `-DALP_PROVISION_FORCE=1`. A new sibling
script, `scripts/program_eeprom_secure_page.py`, builds the 64-byte blob
from the same `board.yaml` + `--serial` + `--mfg-date` inputs
`program_eeprom.py` takes for the 128-byte manifest — a separate script
rather than a mode of that one, since `check_board_id_doc_parity.py`
resolves `program_eeprom.py`'s five length constants by exact name and a
second, differently-sized set risks colliding with that gate's parser.

Graceful degradation is unchanged from `eeprom_24c128_read_identity()`'s
existing contract: on the approved footprint-compatible alternate part
(STMicro `M24128-BFMH6TG`, no second device-select header at all), both new
example modes fail cleanly with a "no second device-select header" message;
the array manifest, and everything that reads it, is unaffected either way.
