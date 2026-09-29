### Fixed — document + apply the host-side retry for the CC3501E's known first-radio-op wedge (#2035)

Roughly 2 in 16 cold boots, the FIRST worker-routed radio opcode of a boot
(a Wi-Fi scan, a BLE enable, a connect -- not `PING`/`GET_VERSION`/
`GET_DIAG_INFO`, which succeed regardless) times out (`ALP_ERR_TIMEOUT`),
and the link then reads `ALP_ERR_IO` until a cold cycle. This was reviewed
as an architecture question: the bridge firmware deliberately does not fix
it in-band -- every candidate firmware-side move has a demonstrated
wedge/brick precedent, recorded in `cc3501e-bridge-firmware`'s
`prebuilt/CHANGELOG.md` -- so the agreed answer is host-side.

`cc3501e_hard_reset()`'s doc now states the rule where every caller finds
it: if the FIRST radio op of a boot fails with `ALP_ERR_TIMEOUT`, call it
ONCE and retry that same op ONCE -- taking 2 in 16 to roughly 1 in 128, the
same warm-nRESET primitive the Puya cold-boot workaround already uses. A
second failure is a real failure and must be surfaced, not retried again;
a LATER op failing the same way is a different condition and must not be
papered over.

This is deliberately NOT put inside any driver op function -- a hard reset
drops every association, BLE link and open socket, so a caller who asked
for one thing must never silently lose unrelated state to a reset it never
asked for. It's applied in exactly one place: `aen-cc3501e-companion-tour`'s
`cc3501e_wifi_scan()` call, the tour's own first radio op, with every later
radio call in that example left untouched.
