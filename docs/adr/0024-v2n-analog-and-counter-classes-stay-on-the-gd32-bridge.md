# 0024. V2N/V2M analog and counter classes stay on the GD32 bridge

Status: Proposed
Date: 2026-08-05

## Context

On V2N/V2M, five portable peripheral classes — ADC, PWM, DAC,
counter, and quadrature encoder (qenc) — are served entirely by the
GD32 IO-MCU bridge. All five backend registrations carry
`silicon_ref = "renesas:rzv2n:n44"`, the same silicon tag as any
native RZ/V2N backend would use: `src/backends/adc/gd32_bridge.c:156`,
`src/backends/pwm/gd32_bridge.c:215`,
`src/backends/dac/gd32_bridge.c:154`,
`src/backends/counter/gd32_bridge.c:106`,
`src/backends/qenc/gd32_bridge.c:66`. The hardware decision behind
this routing is dated 2026-05-12 and is recorded in the supervisor's
own Kconfig help text (`zephyr/kconfigs/v2n-supervisor.kconfig:14-17`):
"per the 2026-05-12 hardware decision, every E1M-standard analog +
counter peripheral on V2N is reached via the GD32 IO MCU bridge."

Issue #1150 asks whether a native RZ/V2N leg should exist alongside
the bridge for pads that are not E1M-standard-routed, selected by
backend priority. This ADR answers that question: no, and the reason
is routing, not preference.

The SoC declares substantial unused analog/counter silicon that a
native leg would have to target: 16× 32-bit GPT timers
(`metadata/socs/renesas/rzv2n/n44.json:73`), 8× CMTW 32-bit timers
(`:74`), 8× GTM 32-bit timers (`:75`), and 24× 12-bit ADC channels
(`:71`). None of it is wired to an E1M-standard analog or counter
pad (see below). The only GPT instance in use anywhere in the tree
is `gpt1_2`, driving the LCD backlight PWM on the Linux side
(`meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-som.dtsi:125-157`)
— a board-specific, non-portable use, not an E1M peripheral.

## Decision

**No native RZ/V2N leg is added for ADC, PWM, DAC, counter, or qenc.**
All five classes stay served exclusively by the GD32 bridge on
V2N/V2M. The evidence below shows this is not a preference between
two working paths — no SoC pin reaches an E1M-standard analog or
counter pad, so a native leg would have nothing to attach to.

### 1. Pad ownership, verified at the TSV source

This matters and must be said plainly: `metadata/pinmux/v2n.yaml`
carries `e1m_pad: "TBD"` on every row and states at `:8` that "the
TSVs remain the single source" — the generated projection alone
would not settle the question. Checked at the source:

- `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv` carries all 26
  E1M analog/counter functions, all routed to GD32 pads:
  - `:12-19` — `ENC0_X PA0`, `ENC0_Y PB3`, `ENC1_X PC6`, `ENC1_Y PC7`,
    `ENC2_X PB6`, `ENC2_Y PB7`, `ENC3_X PB2`, `ENC3_Y PA1`
  - `:20-27` — `PWM0 PA11`, `PWM1 PB1`, `PWM2 PB14`, `PWM3 PC5`,
    `PWM4 PC10`, `PWM5 PC11`, `PWM6 PC12`, `PWM7 PD0`
  - `:28-35` — `ADC0 PD9`, `ADC1 PB12`, `ADC2 PE13`, `ADC3 PE11`,
    `ADC4 PC4`, `ADC5 PA5`, `ADC6 PA2`, `ADC7 PA3`
  - `:36-37` — `DAC0 PA4`, `DAC1 PA6`
- `metadata/e1m_modules/v2n/renesas-peripheral-map.tsv` — the wider
  `grep -inE '(adc|dac|pwm|enc|gpt|gtm|cmtw)'` sweep over the whole
  file returns exactly **one** row: `:6` `BL_PWM PA5`, the on-module
  backlight. The generated projection agrees it carries no E1M
  function: `metadata/pinmux/v2n.yaml:18` lists that same silicon pad
  with `e1m_function: "TBD"`.
- The generated projection agrees:
  `metadata/pinmux/v2n.yaml:169-194` lists all 26 rows with
  `owner: "gd32"`, and no `owner: "renesas"` row carries an E1M
  analog or counter function.

### 2. DAC settles it independently

`metadata/socs/renesas/rzv2n/n44.json` has **no `dac` key** anywhere
in its `peripherals` block — the SoC spec models no DAC. The
portable `alp_dac` surface exists on V2N *only* because of the
GD32. Any "prefer native where available" policy needs an exception
on line one for DAC, which is the policy telling you it is wrong for
the other four classes too.

### 3. Why a fallback leg would be a correctness bug, not dead weight

`include/alp/e1m_x_pinout.h:131-159` defines the portable IDs as
connector positions — the ADC channel comment states plainly:
"Single-ended ADC channels (ANA_S0..ANA_S7 => ADC0..ADC7)"
(`:135`), i.e. `ALP_E1M_X_ADC0` **is** `ANA_S0`, a specific edge pad.
A native leg that cannot reach that pad does not degrade gracefully
— it silently redirects `alp_adc_read(ALP_E1M_X_ADC0)` off the
connector to an unrouted or on-module SoC channel, converting a
loud, recoverable `ALP_ERR_IO` / `ALP_ERR_BUSY` into a quiet
wrong-wire read. That is the central argument against a
priority-selected fallback: it cannot fail safely, because the
"native" side of the choice is not actually wired to the thing the
caller asked for.

Counter is padless — it free-runs on the GD32
(`src/backends/counter/gd32_bridge.c:49`), so no connector pad is
even involved on that one class; the pad argument does not apply to
it, but the "no second untested silicon path" argument in
Consequences still does.

### 4. The resilience question, answered separately

#1150 also argues that the six quarantined bridge surfaces take the
whole class down with them. **That premise is stale, and the record
says so — though not as completely as "cleared" would suggest.** The
six-surface quarantine (`pwm_capture`, `adc_stream`, `qenc`, `tmu`,
`ota_get_state`, `trng`) is the 2026-06-04 first-silicon record
(`examples/v2n/v2n-gd32-bridge-hil-soak/README.md:42-51`). All 20
soak rows carry `quarantined = false` today
(`examples/v2n/v2n-gd32-bridge-hil-soak/src/main.c:745-764`), but
that means *enabled for re-test*, not *passing*: the soak file's own
comment says `adc_stream` was un-quarantined "for the supervised
third pass," with a clause to re-quarantine it if the link still
rots (`examples/v2n/v2n-gd32-bridge-hil-soak/src/main.c:727-735`).
Of the six, the firmware v0.2.3–v0.2.9 campaign (shipping firmware
is v0.2.11, `gd32-bridge-firmware:firmware-version.txt`) names four
as cleared — `pwm_capture`, `adc_stream`, `qenc`, `tmu`
(`gd32-bridge-firmware:README.md:75-78`). `trng` is separate and
weaker than "cleared": its unit still takes intermittent seed
errors and parks with latched flags set; firmware now detects that
state, fails the affected call fast, and rebuilds on the next call —
mitigated, not cleared
(`examples/v2n/v2n-gd32-bridge-hil-soak/src/main.c:717-726`).
`ota_get_state` is not itemized as cleared in either source. Five of
the six original "-5 from cycle 1" failures were traced to one
transport bug — a staging-cursor rewind defect in `transport_spi.c`
that made every re-read fail permanently once a slow handler missed
its first reply window
(`examples/v2n/v2n-gd32-bridge-hil-soak/src/main.c:698-706`).

Genuinely still open, and this ADR does not claim otherwise: not an
open defect class, but an unsoaked firmware delta.
`gd32-bridge-firmware:README.md`'s Status block still names an "ADC
DSP-chain runtime dispatch...degrades to error statuses" gap, but
that block is pinned at v0.2.7 vintage and is stale as a description
of current state — shipping firmware is v0.2.11, which added ADC DSP
dispatch among other changes. The accurate residual risk, verbatim
from `docs/verification-status.md`, is that the bridge "has since
moved to fw v0.2.11 / protocol v0.9 (ADC oversample + resolution,
PWM center-align, ADC DSP dispatch, OTA Path-A hardening)" and "that
delta has not been re-soaked." That is a soak-coverage gap, not a
known defect. `adc_stream`'s zero-sample failure is *not* part of it
— the v0.2.3 CHANGELOG entry that called it "still open" was
superseded by v0.2.4, which root-caused it (`CTL1.DDM` never set,
continuous/DMA controls programmed onto an already-running
converter, the `RCU_DMAMUX` clock never enabled) and fixed it on
silicon.

The real single point of failure is the one supervisor singleton and
one SPI/I2C transport shared under all five classes
(`zephyr/kconfigs/v2n-supervisor.kconfig:11-18`) — not the six
surfaces #1150 names. What bounds that risk today: the 20-row HIL
soak scored **253/253** at **fw v0.2.9 / protocol v0.7**
(`docs/verification-status.md`) — a later, cleaner record than the
first-silicon 1526-cycle run this ADR already calls stale above (in
that earlier run only 13 of the 20 surfaces were healthy and `trng`
failed every cycle;
`examples/v2n/v2n-gd32-bridge-hil-soak/README.md:53-59`) — plus the
supervisor's bounded 100 ms acquire timeout, which returns
`ALP_ERR_BUSY` at the portable surface on contention rather than
hanging (`zephyr/kconfigs/v2n-supervisor.kconfig:87-102`).

Conclusion: resilience belongs on the bridge-firmware quality track
— the soak gate, the OTA path, the transport-recovery ladder — not
on a backend-priority track. A native fallback leg would not fix a
transport-level fault; the transport is the shared dependency
underneath *both* legs.

## Alternatives

**A native leg selected by backend priority** (what #1150 proposes).
This is not a hypothetical mechanism: `alp_backend_select_next()`
(`src/backend.c:134-151`) already exists and is in production use —
`src/security_dispatch.c:248` and `:366`,
`src/update_log_dispatch.c:69` and `:79` all walk it to fall through
a failing backend to the next-best one. The two legs here genuinely
overlap on the ranking, too: `zephyr_drv.c`'s wildcard leg and
`gd32_bridge.c`'s exact leg both register for all five classes at
the same `.priority = 100` (Decision §1's sibling evidence in
`docs/portability.md` §4.5). Rejected anyway, on two independent
grounds:

1. The five analog/counter dispatchers deliberately do **not**
   iterate — each calls the single-shot `alp_backend_select()`
   (e.g. `src/adc_dispatch.c:91`), unlike `security_dispatch.c` /
   `update_log_dispatch.c` above. Adopting the fallback pattern here
   would be new dispatch behaviour, not a reuse of existing wiring.
2. The decisive reason: the SoM routes nothing from the RZ/V2N's own
   ADC/timer pins to E1M pads (Decision §1). A fallback mechanism
   selects among backends that are already wired; it cannot conjure
   routing that was never populated on the PCB. Even if the
   dispatchers were changed to iterate, the "native" candidate they
   would fall through to still has no pin to drive — which is the
   correctness argument in Decision §3: a leg that cannot reach the
   connector pad fails silently instead of loudly.

**Exposing the SoC's own GPT/ADC as a natural-name or Linux-side
surface** for on-module timing needs, as `gpt1_2` already is for the
backlight. Compatible with this decision — it lives outside
`<alp/*>`, needs no priority mechanism, and makes no portability
claim. Recorded here as **available later without revisiting this
decision** — not built now, because nothing in this tree asks for
it yet.

## Consequences

Good:
- The contract is explicit and citable: five portable classes on
  V2N/V2M are bridge-served, full stop — no second, partially-wired
  silicon path to reason about per class.
- The pad-ownership evidence (Decision §1) is now recorded in one
  place instead of requiring a fresh TSV dig each time the question
  comes up.

Bad / costs — real, and stated honestly:
- The bridge is the sole serving path for five classes: a single
  point of failure by routing, not by choice.
- There are real capability deltas at the portable surface versus
  what native RZ/V2N silicon could offer if it were reachable: the
  ADC backend advertises `base_caps = 0u` — an SDK-side gap at SDK
  v0.7, not a bridge hardware limit, per the code's own comment
  (`src/backends/adc/gd32_bridge.c:158`,
  `/* no HW oversample/trigger via bridge in v0.7 */`); counter has no
  alarm support — `set_alarm` returns `ALP_ERR_NOSUPPORT` because
  there is no IRQ line from GD32 to the Renesas SoC
  (`src/backends/counter/gd32_bridge.c:83`). There is also bridge-hop
  latency inherent to routing every call through the SPI/I2C
  transport and the supervisor mutex. See
  `docs/portability.md` §4.5 for the customer-facing version of this
  list.

**Revisit triggers, stated explicitly:**
- A PCB revision routes an E1M-standard analog or counter pad
  directly to the RZ/V2N SoC (Decision §1 would need re-verifying at
  the new TSV).
- The GD32 leaves the BOM.
- A bridge-reliability regression the soak gate cannot clear pushes
  the single-point-of-failure cost above the routing constraint's
  benefit.

## See also

- `docs/adr/0011-intra-family-portability.md` — the intra-family
  swap contract this decision's `ALP_E1M_X_*` connector IDs serve.
- `docs/adr/0023-ethernet-out-of-the-alp-surface.md` — opposite
  polarity: 0023 removes a peripheral class *from* the `<alp/*>`
  surface entirely; 0024 keeps five classes *in* the surface while
  fixing which die serves them. Do not fold the two together.
- `docs/gd32-bridge-protocol.md` — the wire protocol underneath all
  five classes.
- `examples/v2n/v2n-gd32-bridge-hil-soak/` — the soak record this
  ADR's resilience argument rests on.
- `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv`,
  `metadata/e1m_modules/v2n/renesas-peripheral-map.tsv` — the TSV
  pad-ownership source of truth.
