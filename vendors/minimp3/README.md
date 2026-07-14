# vendors/minimp3

Vendored copy of **minimp3** -- <https://github.com/lieff/minimp3>,
`master` (no tagged releases upstream), CC0-1.0 (public domain)
licensed.

## Why vendored instead of west-fetched

`west.yml` already carries a `minimp3` project pin (behind the
disabled `extras-tier1` group, pinned to `main` pending a maintainer
SHA audit), but `west update` cannot run against an out-of-workspace
worktree, and the west topdir workspace this SDK
builds against has not fetched it (`ls modules/lib/` -- absent as of
this vendoring). minimp3 is a single-file STB-style header (decode
API declarations always visible; the implementation compiles in
wherever exactly one translation unit defines
`MINIMP3_IMPLEMENTATION` first), so it is vendored the same way as
`etl`/`fmt`/`doctest`/`jsmn` rather than left as a documented gap.
The `extras-tier1` pin remains the audit trail / version
source-of-truth; the build resolves `#include "minimp3.h"` against
the copy here regardless.

## What's here

`include/minimp3.h` is the complete, unmodified upstream single
header, fetched from `master` on 2026-07-10 (no upstream tagged
release exists to pin to -- see the `west.yml` comment on the
`minimp3` project). `LICENSE` is the upstream CC0-1.0 text,
unmodified.

## Consuming it

Exactly one translation unit must `#define MINIMP3_IMPLEMENTATION`
before `#include "minimp3.h"` to emit the decoder body; every other
TU includes it declaration-only.

**`-Wdouble-promotion` gotcha (Task 0 probe finding, 2026-07-10):**
the alp-sdk Zephyr build enables `-Wdouble-promotion -Werror`.
minimp3's unmodified `mp3d_scale_pcm()` implementation (float
`sample` compared against a `double` literal, e.g.
`sample >= 32766.5`) trips it. Since this is verbatim upstream code
(not patched -- see "What's here" above), the translation unit that
defines `MINIMP3_IMPLEMENTATION` must wrap the include:

```c
#define MINIMP3_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#include "minimp3.h"
#pragma GCC diagnostic pop
```

Verified: this is the only warning the vendored header trips on
native_sim/native/64 host GCC; no other pragma is needed.

## Updating

When the maintainer SHA audit lands (see the `west.yml` TBD
comment), pin `revision:` to that SHA, then re-copy
`include/minimp3.h` from that exact commit and refresh `LICENSE`
here in the same commit.

## License

CC0-1.0 / public domain (see `LICENSE`). Public/redistributable per
`classifying-public-vs-internal`.
