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

Also adds `alp_secure_page_mirror_classify()` (`<alp/hw_info.h>`, header-only
`static inline`, following the existing `<alp/protocol/crc16.h>` pattern):
the mirror equivalent of `alp_hw_info_classify_manifest()`, refusing to
parse a page whose `magic`/`schema_version` don't match exactly what this
build understands, in either direction — the same forward-compatibility
rule the spec states. It copies through `memcpy` rather than casting the
raw device buffer, because `eeprom_24c128_identity_t::secure_page` sits at
a 1-byte-aligned offset with no stronger guarantee, and this struct's own
`uint16_t`/`uint32_t` fields need 2/4-byte alignment — a straight cast is a
real misaligned-access bug, not just a style nit. Both provisioning-example
modes route every raw device-page read through it.

`eeprom_24c128_secure_page_lock()`'s post-lock wait (replacing the removed
ACK poll, since a correct lock NAKs any further write to the page) uses
`alp_delay_ms()`, not `alp_delay_us()` — the ~20 ms wait belongs on the
yielding primitive; `alp_delay_us()` is a non-yielding busy-wait documented
for sub-millisecond sequences only, and 20 ms of that stalls every
equal-or-lower-priority thread on the core (issue #1621's defect class).
The wait budget is now its own named `EEPROM_LOCK_WAIT_MS` constant rather
than a runtime `(EEPROM_WRITE_POLL_STEP_US / 1000u) * EEPROM_WRITE_POLL_MAX`
product — that expression is integer division of two independently-tunable
constants, and any `EEPROM_WRITE_POLL_STEP_US` below 1000 rounded the whole
term to 0, silently deleting the wait.

Review before the first production lock (this code permanently locks a
64-byte identity page on customer modules, run without a rehearsal on a
sacrificial part) found and closed three more gaps in
`examples/aen/aen-eeprom-provision` mode 3 (the permanent-lock mode):

- The mirror about to be sealed was never checked against the module's own
  128-byte array manifest at `0x50` — the two are written by separate tool
  runs, in different production steps, from separate `.bin` files, so a
  self-consistent, CRC-valid mirror belonging to another module, or one
  carrying a typo'd `--serial` or the wrong `--mfg-date`, would lock forever
  undetected. Note the limit: `scripts/program_eeprom_secure_page.py`
  imports `scripts/program_eeprom.py` and reuses its `board.yaml` loader, so
  `sku` and `hw_rev` resolve through the same code from the same field —
  generate both blobs from one `board.yaml` and those two fields carry no
  independent signal, and a `board.yaml` that is wrong for the module agrees
  with itself here. `serial` and the mfg date are independent either way.
  Mode 3 now reads the array manifest fresh, integrity-checks
  it (magic + schema_version + CRC — a missing or corrupt manifest refuses
  exactly like a real disagreement, not as "nothing to disagree with"), and
  refuses to lock unless it agrees with the mirror on `sku`, `hw_rev`,
  `serial`, and the mfg date, comparing each as a NUL-terminated string
  (the two objects use different field widths: manifest 24/8/24 bytes,
  mirror 16/12/23) rather than a raw byte range.
- Mode 3's own comment claimed a fresh `read_identity()` call inside a
  single RAM-run "IS the cold-cycle read-back the provisioning doc
  requires" — false: a Flow C RAM-run is a J-Link/OpenOCD halt-load-go over
  SWD that never interrupts the power rail, so an operator could chain
  mode 2 then mode 3 with power continuous the whole time. Mode 3 now
  refuses to lock without a second, separately-named build flag,
  `-DALP_SECURE_PAGE_COLD_CYCLED=1` (distinct from `-DALP_LOCK_SECURE_PAGE`,
  wired the same way in `CMakeLists.txt`) — an operator attestation that a
  real cold cycle (rail confirmed at 0.0 V, not a warm reset) happened
  between the mode 2 write and this run; the actual evidence belongs in the
  bench log's power telemetry, not in this flag.
- `eeprom_24c128_secure_page_lock()` confirms only the Lock Status bit,
  never the payload, so mode 3 now re-reads the page and byte-compares it
  against the intended blob after a successful lock, before ever printing
  `RESULT PASS` — the same check the "already locked" branch already
  performed for a page found pre-locked, now mirrored for the branch that
  just performed the lock itself. A lock-frame bug that also writes INTO
  the page (two such bugs were found and fixed on this driver during
  review) would otherwise set the lock bit, corrupt the payload, and still
  print PASS.

`alp_secure_page_mirror_t` and `alp_secure_page_mirror_classify()` now
carry an explicit `@par ABI status: [ABI-EXPERIMENTAL]` tag (previously
inheriting `hw_info.h`'s file-level `[ABI-STABLE]` by omission) — the
format cannot be frozen while the driver functions that write and lock it
are still `[UNTESTED]` / `[PAPER-ONLY]` against silicon. `docs/abi-markers.md`
updated to match.

Also fixes four pre-existing misaligned-cast reads of `alp_hw_info_eeprom_t`
(the same bug class `099923591` fixed for the Secure Data Page blob, left
unswept on the 128-byte array-manifest path): the array-write mode of
`aen-eeprom-provision`, the two `*-eeprom-manifest*` read/dump examples,
`aen-evk-demo`'s EEPROM-identity phase, and the `eeprom_manifest_fuzz.c`
libFuzzer harness (where the cast additionally defeated the harness's own
purpose — an x86 host tolerates the unaligned read silently, so the bug it
exists to catch on the real ARM target could never trip). All now `memcpy`
into an aligned local instead of casting a `uint8_t[]`/`uint8_t*` buffer.
