### Fixed — close four drift-tolerance holes left by the #2350 anchor fix (#2350)

#2371 made an anchored citation's line number advisory: if the quoted text
was not in the cited range, `_check_one`
(`scripts/check_changelog_citations.py:633`
("def _check_one(frag: Path, text: str, added: set[int] | None = None,"))
fell through to a whole-file search and passed on a unique match. Adversarial
review of that fix found four cases where trusting the match was itself the
mistake:

- **A citation you just wrote gets no grace.** The whole-file fallback exists
  to forgive an UNRELATED line moving under an ALREADY-correct citation. A
  citation on a line ADDED this branch has no such history, so a wrong line
  there is the author's own mistake made right now — trusting a coincidental
  match elsewhere could silently paper over it. Now a hard FAIL naming it
  explicitly instead
  (`scripts/check_changelog_citations.py:762`
  ("newly added citation: anchored on {needle!r}, ")).
- **A short, generic anchor is refused, not trusted.** Uniqueness in TODAY's
  file is not proof the note still means the same thing once the anchor is
  this short: `changelog.d/2051.md`-style, two DIFFERENT citations anchor the
  identical 14-character `"int main(void)"` into a file that genuinely has
  two `main`s — delete either one and the SURVIVOR's citation "resolves
  uniquely" at the wrong line, silently. A new floor,
  `_MIN_DRIFT_ANCHOR_LEN = 16`
  (`scripts/check_changelog_citations.py:586`
  ("_MIN_DRIFT_ANCHOR_LEN = 16")), refuses a unique match shorter than that.
  `--fix` still rewrites one — the human running it can read the line, the
  checker cannot — but marks it `[VERIFY: ...]` and counts the rewrites
  needing a human read in its summary, never silently
  (`scripts/check_changelog_citations.py:1011`
  ("[VERIFY: short anchor {needle!r} ({len(needle)} < ")).
- **A re-anchor that would overflow EOF fails, not passes.** A unique match
  whose cited width cannot fit past that line now fails outright, consistent
  with `--fix`'s own EOF refusal, instead of passing with a dead-end
  "run --fix" note
  (`scripts/check_changelog_citations.py:833`
  ("anchored on {needle!r}, found uniquely at line ")). A shrunken file no
  longer hard-fails an ANCHORED citation on line count alone either — only an
  un-anchored one still does, since there is no anchor text to search the
  whole file with
  (`scripts/check_changelog_citations.py:731` ("if end > len(lines):")).
- **A needle repeated twice on one line is not ambiguous.** `_anchor_line_hits`
  now counts a line at most once, so `foo(x); foo(x);` no longer reports a
  nonsensical two-candidate ambiguity for a citation with exactly one real
  candidate line
  (`scripts/check_changelog_citations.py:611`
  ("if not hits or hits[-1] != line:")).

Also added: `--strict-lines`, the release-time hook that turns an
otherwise-advisory drift into a hard error right before a release cut
freezes `[Unreleased]` into history — a released section is never rewritten
by `--fix` afterward, so a drift left unresolved at exactly that moment ships
wrong forever (the #1387 failure mode again, via the release path instead of
a rebase). Wired into `scripts/bump_version.py`'s `main()`, before any file
is touched
(`scripts/bump_version.py:268` ("subprocess.check_call(")), refusing the
bump and naming `--fix` as the remedy rather than running it automatically.
Never applied to `CHANGELOG.md`'s already-released tail, which stays
unrewritten and unblocked-on either way — its own drift is now printed and
counted separately, labelled `(released history, not blocking)` and never
suggesting `--fix`
(`scripts/check_changelog_citations.py:1408`
("released history: {released_checked} citation(s) graded ")).
