# vendors/jsmn

Vendored copy of **jsmn** -- <https://github.com/zserge/jsmn>,
tag `v1.1.0`, MIT licensed.

## Why vendored instead of west-fetched

`west.yml` already carries a `jsmn` project pin (behind the
disabled `extras-tier1` group), but `west update` cannot run
against an out-of-workspace worktree, and the west topdir workspace
this SDK builds against has not fetched it
(`ls modules/lib/` -- absent as of this vendoring). jsmn is a
single self-contained header (no `.c` companion needed in default
mode -- see `JSMN_STATIC`/`JSMN_HEADER` in the file), so it is
vendored the same way as `etl`/`fmt`/`doctest` rather than left as
a documented gap. The `extras-tier1` pin remains the audit trail /
version source-of-truth for when a real `west update` becomes
available; the build resolves `#include "jsmn.h"` against the copy
here regardless.

## What's here

`include/jsmn.h` is the complete, unmodified upstream single
header. `LICENSE` is the upstream MIT license text, unmodified.

## Updating

Bump the `revision:` in `west.yml`'s `jsmn` (`extras-tier1`) entry,
then re-copy `include/jsmn.h` from the matching upstream tag and
refresh `LICENSE` here in the same commit.

## License

MIT (see `LICENSE`). Public/redistributable per
`classifying-public-vs-internal`.
