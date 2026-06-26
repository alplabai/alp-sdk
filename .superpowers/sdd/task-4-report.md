# Task 4 Report — AEN401 xHCI USB-host disposition: supersede DWC2, docs, PR re-point

**Status:** DONE
**Date:** 2026-06-26
**Branch:** `feat/aen401-usb-host`
**Commit:** `10036a85` — `docs(aen401): xHCI USB-host disposition — supersede DWC2, grounding note, CHANGELOG`

---

## Doc changes

### `docs/superpowers/plans/2026-06-26-aen401-usb-host.md`

Added the required blockquote banner as the first line:

```
> **SUPERSEDED** by 2026-06-26-aen401-xhci-usb-host.md (the IP is xHCI, not DWC2).
```

### `examples/peripheral-io/usb-host-storage/README.md`

Full rewrite to replace DWC2 with xHCI:
- `uhc_dwc2_alif` → `uhc_xhci_alif`
- "placeholder values" for USB address → real `0x48200000` / IRQ 101 (DFP-grounded)
- Status line: "Compile-only skeleton; enumeration bench-gated" (honest framing)
- "What it does" section: references `zephyr_uhc0` at `0x48200000`
- Bench bring-up checklist: rewritten for xHCI (DWC3 GCTL/DCTL init, cap-reg
  read, DCBAA/ring allocation, port-reset, IRQ 101 wiring, TRB scheduling, event ISR)

### CHANGELOG

No change needed. Task 2 already produced a correct xHCI entry with no DWC2
references (confirmed by grep: zero matches for `dwc2|DWC2` in the Unreleased
section). Entry was consolidated/accurate; no duplication added.

### `docs/superpowers/notes/2026-06-26-aen401-xhci-usb-grounding.md` (created)

New file (force-added: `notes/` is gitignored for scratch, but this is a
deliberate SDK artifact). Contents:
- Context: why DWC2 was wrong; DFP showed DWC3-family xHCI
- No-board build evidence (ELF arch, flash/RAM numbers from Task 3 report)
- DFP grounding table (USB base / IRQ / GCTL offset — facts only, no HWRM prose or OneDrive path)
- Full TODO(aen401-bench) bring-up list (DWC3 init, xHCI startup, port reset,
  IRQ wiring, TRB scheduling, event ISR)
- Artifact index

---

## doc-drift result

```
doc-drift: OK (no dead symbol refs, docs index complete).
```

---

## Push + PR result

```
git push origin feat/aen401-usb-host  →  cc53f411..10036a85  (exit 0)
gh pr edit 268 --title "AEN401 M55/Zephyr USB host (xHCI, on-SoC DWC3-family)" --body-file ...
gh pr ready 268  →  Pull request #268 is marked as "ready for review"
```

**PR #268 final state:**
- Title: `AEN401 M55/Zephyr USB host (xHCI, on-SoC DWC3-family)`
- Draft: false
- State: OPEN (not merged — left for the maintainer)

PR body: IP-correction banner at top explains the DWC2→xHCI pivot; lists all
components (e4/e7/e8 metadata, board, driver, backend, example); build result;
bench-gated TODO list; doc-gate results. No AI footer; no confidential prose.

---

## Concerns / notes

- The `notes/` directory is gitignored by `.gitignore:113` (`notes/` pattern).
  The grounding note was committed with `git add -f` as the brief explicitly
  names `docs/superpowers/notes/`. Future files there also need `-f`.
- No Co-Authored-By footer; no AI footer in PR body (per constraints).
- PR body and committed files contain no HWRM prose, no OneDrive paths, no
  confidential document references — only DFP `soc.h` facts.
- PR is ready for review; maintainer decides when to merge.

---

## Final-review fixes (2026-06-26)

**Fixes applied:**

1. `examples/peripheral-io/usb-host-storage/src/main.c` — replaced all three DWC2 residues:
   - Block comment: `uhc_dwc2_alif` / "DWC2-host" → `uhc_xhci_alif` / "xHCI"
   - printf: `alif,dwc2-uhc DT node` → `alif,xhci-uhc DT node`
   - printf: `DWC2 bring-up required` → `xHCI bring-up required`

2. `zephyr/Kconfig` line 3192 — `DWC2 USB host` → `xHCI USB host` in section comment.

3. `examples/peripheral-io/usb-host-storage/board.yaml` — reworded "placeholder reg/IRQ values" to "DFP-grounded 0x48200000 / IRQ 101".

4. `docs/superpowers/specs/2026-06-26-aen401-xhci-usb-host-design.md` — dropped `(OneDrive)` parenthetical from the Confidential-source rule.

**Verification:**
- `grep -niE "dwc2"` over the example dir + Kconfig: CLEAN (no matches).
- clang-format-22 `--dry-run --Werror` on main.c: CLEAN.
- `py -3.14 scripts/check_doc_drift.py`: `doc-drift: OK`.
- No west rebuild required (changes are comment/string-only).
