# The shape of `tan`: one tool, content-driven, honestly reproducible

**Status:** agreed 2026-07-30. Supersedes the informal "tan is west for alp-sdk"
framing used earlier in the port.

## The decision in one line

`tan` is the only tool an Alp Lab customer ever names. `alp-sdk` is content.
The planner engine lives in `tan`; every hardware fact and every
family/vendor/part rule lives in `metadata/**` and is resolved at runtime.

## Why this is not west/Zephyr, and why that is deliberate

The port was originally motivated by the west/Zephyr coupling, and the mechanism
we borrowed is real: a separately-installed, separately-versioned tool, plus a
content repo that declares its contributions (`scripts/west-commands.yml`,
`zephyr/module.yml`, `metadata/emit-registry-v1.json`).

But west contains **zero** Zephyr knowledge, and `tan` now contains the planner.
That difference is intentional. Keeping the planner in the SDK is what created
the cross-repo contract this port exists to remove: two languages, two release
cadences, and an emit surface that had to be kept byte-identical across a seam.
Choosing "not west" is the whole point; the cost of that choice is priced in
pillar 3 below, not wished away.

One piece genuinely is west-shaped and stays: `zephyr/module.yml:40-42` declares
`scripts/west_commands/runners/alif_flash.py` and `rzv2n_mtd_flash.py` to
**Zephyr**, and six board files wire them as defaults —
`e1m_v2n101_m33_sm/board.cmake:31` and `e1m_v2m101_m33_sm/board.cmake:31` use
`board_set_flasher_ifnset(rzv2n_mtd_flash)`, and four AEN boards
(`e1m_aen401_m55_hp:19`, `e1m_aen601_m55_hp:19`, `e1m_aen801_m55_hp:23`,
`e1m_aen801_m55_he:53`) use `alif_flash`. Those are Zephyr's plugin contract,
not ours. They remain Python in alp-sdk regardless of how far the port goes, and
that is not a failure of the removal.

## Pillar 1 — one vocabulary

A customer learns exactly one tool: `tan`. `west`, `cmake`, the Zephyr SDK and
the workspace venv are things `tan` installs and drives, never things a customer
is asked to type.

This is ~90% true today (22 registered commands, and `cmake/alp.cmake` now
requires `tan` with no Python fallback). The remaining leaks are specific:

- `west flash` is the wired default flasher on the V2N101/V2M101 and AEN boards.
  The runner files stay (Zephyr's contract); what changes is that `tan flash` is
  the only **documented** path, and `tan` drives the runner rather than the
  customer invoking `west` directly.
- `west alp-emit` exists as a parallel front door onto the emit surface.
  `docs/cli.md:122-131` already records that `tan generate` and `west alp-emit`
  cover **disjoint** artefact sets — that disjointness is the bug, not a feature,
  and it resolves by retiring `alp-emit` as a customer surface once
  `tan.planner_cli` covers what it served.
- Docs still show `west build` in places.

**Acceptance:** a customer can go from `tan sdk install` to a blinking LED
without typing `west`, `cmake`, or a venv activation, and no document instructs
them to.

## Pillar 2 — flexibility by content

The cost boundary is **not** the SKU. `metadata/e1m_modules/E1M-V2N102.yaml` is
literally *"memory variant of V2N101"* — identical `family: renesas-rzv2n`,
`silicon: renesas:rzv2n:n44`, `silicon_variant: R9A09G056N44GBG`. A new SKU is
one preset YAML and `tan` is already uninvolved. There are 11 such presets today.

The boundary is one level up:

| Level | Example | Cost |
|---|---|---|
| SKU | a memory variant of a shipping SoM | one preset YAML — already free |
| SoM on a known SoC | new module, `silicon:` already in `metadata/socs/` | preset, possibly a SoC variant entry — metadata |
| **SoM on a new SoC or vendor** | a new `metadata/socs/<vendor>/<family>/<part>.json` | **`tan` branches today** |

**The rule:** `tan` never branches on a family, vendor or part. It resolves them
from metadata.

Three branches violate it, all already tracked as DEBT by
`python/tests/gates/test_no_new_hardware_facts.py`:

- `tan/core/scaffold.py` — `_FAMILY_TREES`, `DEFAULT_SOM_SKU`,
  `IOT_STARTER_SUPPORTED_SKU`, and `sku.startswith(('E1M-V2N','E1M-V2M'))`
- `tan/core/renode_plan.py` — `_SKU_TOKEN_TO_FAMILY`, mapping `AEN`/`V2N`/`V2M`/
  `NX9` to Renode platforms
- `tan/planner/kconfig.py` — `CONFIG_ALP_SDK_WIFI_*`, keyed on the wireless chip

Note `scaffold.py`'s branch is constrained by invariant I-32: `tan init` is
deliberately SDK-checkout-free and reads a vendored capture, so its family
mapping cannot simply read `metadata/`. Retiring that one means declaring the
template catalogue's family mapping in the vendored capture itself, not pointing
it at an SDK root.

**Delivery:** a new SoC ships in an alp-sdk release; the customer runs
`tan sdk install <version>`. That command already exists
(`list | install | current | switch`, cache at `~/.alp/sdk-cache`, `--global` for
a machine-wide pin). `tan new-som` is the vendor-side onboarding command.

**Acceptance:** onboarding a new SoC/vendor requires **no `tan` release**, proved
by doing it — add a synthetic vendor to `metadata/socs/`, scaffold and plan
against it with an unmodified `tan`.

## Pillar 3 — tiered, honest reproducibility

Byte-identical *firmware* is not achievable and we will not claim it: the Zephyr
SDK, gcc and Zephyr itself move independently of anything we pin. Two different
things get conflated today, and a customer cannot tell them apart:

- **Generated config** — `alp.conf`, the board tree, the build plan. A function
  of the tan planner and the SDK metadata. Both pinnable, so this **can** be
  byte-identical.
- **The compiled binary** — cannot be, ever.

The port made generated config a function of `f(tan version, SDK metadata)`
where it used to be `f(pinned SDK)` alone, and `tan` is installed globally,
outside every project pin. That is the real regression this pillar addresses.

**Mechanism:** stamp three identities into every materialised artefact and into
`scripts/build_receipt.py`'s output — tan version, alp-sdk revision, toolchain
(Zephyr SDK + Zephyr revision). On rebuild, compare all three and report them
**differently**:

- planner or metadata drift → **loud**: your generated config will differ
- toolchain drift → **informational**: your binary will differ, your config will
  not

The distinction is the deliverable. A customer who sees a difference today cannot
tell which happened; after this they can, and only one of the two is actionable
by pinning.

**Acceptance:** rebuild a project with a deliberately different `tan` and confirm
the config-drift warning fires and names both versions; rebuild with a different
Zephyr SDK and confirm it does **not** fire, but the toolchain note does.

## Explicitly rejected

- **Planner back in alp-sdk** (true west/Zephyr shape). It would give
  reproducibility by construction, but reinstates the two-language cross-repo
  contract this port exists to remove.
- **Byte-identical firmware from a pin.** Not achievable; promising it would be
  a lie that surfaces at certification time.
- **Removing the Zephyr runner files.** They are Zephyr's contract, wired as
  board defaults. Removing them breaks `west flash` on shipping boards with
  nothing in `tan` able to substitute.
- **"Whatever is cheapest per launch."** That is how the literals the I-26 gate
  now tracks as DEBT entered `tan` in the first place.

## Scope

The three pillars are independent and should NOT be one implementation plan.
Each has its own acceptance test, its own files, and its own blocking condition;
bundling them would produce a change nobody can review and a gate nobody can
attribute. Pillar 2 is the one with real design work left (the I-32 constraint on
`scaffold.py`); pillars 1 and 3 are mostly mechanical once their blockers clear.

Ordering is not arbitrary. Pillar 1's `alp-emit` retirement is blocked on
`tan.planner_cli` covering `scaffold`, which is the same blocker as deleting
`scripts/alp_project.py`. Pillar 3 should land before any release that a customer
might later need to reproduce, because a stamp added afterwards cannot describe
builds made before it.

## Risks

- Pillar 2's `scaffold.py` item is the hardest, because I-32 forbids the obvious
  fix (read metadata). It may need the vendored capture to carry its own family
  declaration, which is a format change.
- Pillar 3's stamping touches the build receipt, which is part of the Piece-5
  traceability story; it must extend that rather than fork it.
- Pillar 1's `alp-emit` retirement is blocked until `tan.planner_cli` covers the
  modes it serves. As of `63c2c6f` that is 40 of 44 emit cases; `scaffold` is the
  gap.
