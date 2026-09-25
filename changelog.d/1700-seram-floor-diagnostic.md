### Added — a diagnostic warning when a module's SERAM sits below the ADR-0030 floor (#1700)

`docs/aen-se-services.md` section 0.1 and ADR 0030 record the SERAM/services-
library pairing rule and the v110 floor, but until now that check was purely
manual: a reader had to call `se_service_get_se_revision()` themselves and read
the banner. `src/backends/soc_info/alif_se.c` now runs that same check on every
`alp_soc_info_read()` / `alp_soc_secure_fw_ping()` and logs one `LOG_WRN` the
first time it sees a SERAM version below v110, naming ADR-0030 and #1700.

This is a **diagnostic, not a fix**. The customer report behind #1700 is a
mismatched SERAM (v106) against a services library from SETOOLS v109: the first
SE service request stopped HFXTAL and unlocked the PLL, and M55-HP fell from
400 MHz to 76.8 MHz and stayed there. Alif confirmed a real API break below v109
for E8; across a break of that kind any SE behaviour is possible, so this
backend does **not** attempt to re-establish the clock itself -- the same
"no workaround is safe to ship" reasoning `docs/aen-se-services.md` already
states for a mismatched pair rules that out. What it does instead is turn a
silent 76.8 MHz stall into an actionable, one-shot log line, so a mismatched
module gets caught before a customer spends time debugging an application on
top of undefined SE behaviour.

The check only runs on this backend's own read path -- whichever `se_service_*`
call an application issues first (the customer's own trigger was
`SERVICE_CRYPTOCELL_GET_RND`, not a soc-info read) is not intercepted, so
`docs/aen-se-services.md` still leads with calling `se_service_get_se_revision()`
explicitly as the first diagnostic step regardless of whether the warning fired.

`tests/unit/alif_se_seram_floor_warning/` adds white-box native_sim coverage
(the `#include`-the-real-TU technique `tests/unit/se_cryptocell_hash_bounds/`
already uses) for the banner parser and the warn-once behaviour, on both the
customer's failing capture (SERAM v106) and the healthy reference-board capture
(SERAM v110).

Still open, unchanged by this PR: whether the API break *causes* the HFXTAL/PLL
drop is an Alif-side question this repo cannot answer from software alone --
see #1700 for the live escalation and the bench plan to demonstrate it.
