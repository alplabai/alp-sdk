# Capturing a bench-measured model perf point

How to produce one file under `metadata/model_perf/` — a **measurement** of one
model, on one accelerator target, of one SoM revision, by one named toolchain.

This recipe exists **before** any bench time is spent on it, on purpose. A perf
point is worth exactly what its reproducibility is: a customer with no NPU
toolchain and no silicon reads it as the exact answer for their module, on our
authority, with nothing of their own to check it against. If a second person
cannot re-derive a number from this page, the number should not have shipped.

Contract: [`metadata/schemas/model-perf-v1.schema.json`](../../metadata/schemas/model-perf-v1.schema.json).
Gate: `python3 scripts/validate_metadata.py` +
`python3 -m pytest tests/scripts/test_model_perf_metadata.py`.

---

## 0. Read this before booking bench time

Two hardware questions are **open**, and both change what the numbers mean. A
campaign run before they are answered has to be recaptured. They are recorded
in full in
`docs/superpowers/plans/2026-08-16-model-perf-tier2.md` under "Open questions
for the maintainer"; quoted here because this page is where a bench operator
will be standing.  (Cited as a path rather than a link on purpose: the
`doxygen · public headers` gate excludes `docs/superpowers/` from its INPUT,
so a markdown link there becomes an unresolvable `\ref` and fails the build.)

**(a) The vela profile question — which machine are we measuring?**
Proven against this checkout's own fixture, not reasoned from the metadata
alone — run this before booking bench time and read the output, don't take
the paragraph below on faith:

```
vela --accelerator-config ethos-u85-256 --memory-mode Sram_Only \
     --output-dir <tmp> tests/fixtures/models/person_detect_int8.tflite
```

with **no `--config` at all** exits `rc=0`, its network summary reports
`Memory mode  Sram_Only` and `Total SRAM used  72.00 KiB`, and it prints,
verbatim:

```
Warning: No configuration file specified. Using a default of ['<venv>/ethosu/config_files/Arm/vela.ini']. Compilation may be invalid or non-optimal.
Warning: No system configuration specified. Using a default of Ethos_U85_SYS_DRAM_Mid. Compilation may be invalid or non-optimal.
```

`Sram_Only` is one of vela's own Arm **built-in** `Memory_Mode` sections
(`vela --list-configs Arm/vela.ini` lists it beside five siblings — no vendor
`.ini` needed to reach it), so **a correct arena figure alone does not prove
`ensemble_vela.ini` was used.** `system_config` is a different story: with
`--config`/`--system-config` not both given, it silently falls back to
`Ethos_U85_SYS_DRAM_Mid` — one of eleven Arm built-in `System_Config`
sections in the same `Arm/vela.ini` — which is **DRAM-backed**, a machine the
E8 does not have. Alif's own SRAM-only sections (`Ethos_U85_SRAM_Only` and its
per-core siblings) live only in the proprietary `ensemble_vela.ini`, which
alp-sdk does not redistribute. `memory_mode` and `system_config` are not the
same claim: `memory_mode` governs **placement** (where the arena lives, and
the E8's correct choice happens to coincide with an Arm built-in), while
`system_config` governs **bandwidth and timing** — so it is `system_config`,
not `memory_mode`, that decides whether a latency figure describes this SoM
or a DRAM-backed one that shares none of its numbers.

*What this means for `scripts/validate_metadata.py`.* Rule 14 requires a
point's recorded `toolchain.memory_mode` (and `toolchain.system_config`,
**only where the part's own SoC spec declares a `system_config` value of its
own**) to equal the module's SoC spec's declared `npu_toolchain.vela`
profile. The E8's `npu_toolchain.vela` (`metadata/socs/alif/ensemble/e8.json`)
declares `memory_mode: "Sram_Only"` and
`system_config_requires_vendor_config: true`, but **no `system_config` value
of its own** — a `System_Config` section describes one core subsystem's
memory view, and the E8 carries three distinct Ethos-U accelerators, so the
SoC-level block is deliberately silent on which one. Rule 14 only compares
fields the SoC spec DECLARES: it checks `memory_mode` (and refuses
`Dedicated_Sram_384KB` or any other mismatch there), but with nothing declared
for `system_config` it has nothing to compare that field against. **On its
own, rule 14 does not require `ensemble_vela.ini`.** A point could record the
E8's correct `memory_mode: "Sram_Only"` next to
`system_config: "Ethos_U85_SYS_DRAM_Mid"` — vela's own flagless default — and
rule 14 would pass it: a correct arena beside a latency modelled on a
DRAM-backed machine the part does not have, at `confidence: "certain"`,
undetected. That is the quieter sibling of the 5.3x SRAM overstatement rule 14
exists to catch, one field over — and rule 14 alone cannot see it.

**Rule 15 closes that hole.** Where the SoC spec flags
`system_config_requires_vendor_config: true`, a point's
`toolchain.system_config` must NOT be one of vela's own Arm built-in
`System_Config` names — checked against the flag, not against a value the
spec may not declare (`_VELA_BUILTIN_SYSTEM_CONFIGS` in
`scripts/validate_metadata.py`, measured against `ethos-u-vela` 5.1.0's own
`Arm/vela.ini` via `vela --list-configs`). **A point captured without
`ensemble_vela.ini` on a part that requires one is refused at publish time**,
with no override field: a genuinely off-profile experiment belongs in the raw
capture log `capture.reference` already cites, never in
`metadata/model_perf/`.

This makes `ensemble_vela.ini` — Alif-proprietary, gitignored
(`.gitignore:150-155`), not redistributed by alp-sdk — a **hard prerequisite
for any Ethos-U capture on the E8**: without it on the compiling machine, vela
falls back to `Ethos_U85_SYS_DRAM_Mid`, rule 15 refuses the resulting point,
and no Ethos-U measurement can be published from that run. **Get
`ensemble_vela.ini` before you book bench time**, not after a capture is
refused.

*Recognise the failure at the terminal, not at review.* Compiling without
`--config` still exits `rc=0` — vela does not fail the compile — and prints
exactly the two `Warning:` lines quoted above, so nothing forces a look at the
output. A point built from that run records
`system_config: "Ethos_U85_SYS_DRAM_Mid"`, and `validate_metadata.py`'s rule
15 refuses it at publish time, not at bench time — by then the run, and the
bench reservation that produced it, are already spent. Read the vela output
for those two lines before trusting any figure from the run.

**(b) The const-region question — what does `req_sram_kib` actually count?**
`req_sram_kib` today reports the **arena alone** (the plan records 72 KiB
measured for `person_detect_int8.tflite`) while roughly 307 KiB is genuinely
SRAM0-resident. A bench point should record what is REALLY resident, and that
needs the per-part answer. Until it exists: **omit `req_sram_kib`** rather than
publish the arena under a name that promises more, and say so in `notes`.

---

## 1. Fix the identity before you measure anything

A perf point is pinned to what produced it, or it is a lie. Write these down
first; every one of them ends up in the file **and** in its path:

| Identity field | Where it comes from |
| --- | --- |
| `measured_on.sku` | the module in the fixture, e.g. `E1M-AEN801`; must exist as `metadata/e1m_modules/<sku>.yaml` |
| `measured_on.hw_rev` | READ off the module in front of you, never typed from memory or copied from a label (§1a); the **revision key** (`r1`, `r2`, …) it reads back, which must also exist in `metadata/e1m_modules/<family-dir>/hw-revisions.yaml` — not the Altium `board_rev` string |
| `measured_on.core` | the core that drove the inference, keyed as in the SKU preset's `topology:` (`m55_hp`, `m55_he`, `a32_cluster`, `a55_cluster`, `m33_sm`, `m33`) |
| `measured_on.backend` | `ethos_u` \| `drpai` \| `deepx_dxm1` \| `cpu` |
| `measured_on.accel_config` | e.g. `ethos-u85-256`; `""` for a backend with no such knob |
| `model.sha256` | `sha256sum` of the **exact bytes you compile**, before any toolchain touches them |
| `model.source` | where those bytes came from — a repo-relative path if the model is in-tree, otherwise a `<store>:<path>` citation or a URL |
| `toolchain.name` + `.version` | the compiler you ran, and the version it reports |
| `toolchain.system_config` + `.memory_mode` (+ `.pins`) | the **profile** the compile ran under (§2) |

`(backend, accel_config)` must be one the SKU actually resolves — derived from
the host SoC's `npus[]` plus any on-module discrete accelerator SoC whose
`variants[].alp_module_skus` lists the SKU. The metadata gate refuses a pair
the module does not have, and refuses a `core` the SKU's `topology:` does not
declare. Where the SoC spec pairs an accelerator to a specific core
(`metadata/socs/alif/ensemble/e8.json` pairs the Ethos-U55 high-perf to
`m55_hp` and the high-efficiency to `m55_he` via `npus[].paired_core`) the gate
also refuses a point that contradicts the pairing. Where it declares none — the
E8's Ethos-U85 today — nothing is inferred and nothing is checked: **do not
invent the pairing in your point either**; record the core you actually ran on.

If the module in front of you is not at a revision the family table knows,
**stop**: add the revision to `hw-revisions.yaml` first. A point tagged with
the wrong revision is worse than no point. `hw-revisions.yaml` is deliberately
open-ended — `hw_rev` is EEPROM-provisioned per module at production test
([`docs/board-id.md`](../board-id.md)), so a legitimately-provisioned new
revision must never be rejected by a fixed enum; a revision the table does not
yet know is an omission in the table, not a defect in the module.

**§1a. `hw_rev` is READ, never typed.** The revision lives on the module, not
in anyone's memory: it is provisioned once into the on-module 24C128 EEPROM's
128-byte manifest at production test
(`scripts/program_eeprom.py`) and is the sole authoritative source of SoM
hardware revision — there is no SoM-side ADC cross-check
([`docs/board-id.md`](../board-id.md)). A hand-typed digit transposes; a read
does not. Before recording `measured_on.hw_rev`, read the manifest off the
exact module on the bench:

`examples/bringup/board-selftest`'s **source** is portable across every E1M
family (no chip driver — `docs/portability.md`), but its `board.yaml` ships
pinned to `som.sku: E1M-AEN801` / `preset: e1m-evk` (the example's own header
comment already says as much), and `-b <your-board-target>` selects the
Zephyr board CMake target only — it does not touch `som.sku`.
`scripts/alp_project.py` reads `som.sku` straight from `board.yaml`
(`CMakeLists.txt:16`), so on any module other than an E1M-AEN801 the command
below configures for the wrong SoM and either fails to configure or (worse)
silently builds a point for the wrong module. **If the module on the bench
is not an E1M-AEN801, edit `som.sku` (and `preset:`, if you are not on an
EVK carrier) in `examples/bringup/board-selftest/board.yaml` first.**

```bash
# Portable Ring-1 example, no chip driver (docs/portability.md).
# board.yaml ships pinned to som.sku: E1M-AEN801 / preset: e1m-evk --
# edit both fields first if that is not the module on your bench.
west build -b <your-board-target> examples/bringup/board-selftest
west flash
```

Copy the `rev <hw_rev>` token from the printed
`[selftest] SoM identity: <sku> rev <hw_rev> sn <serial> -> PASS` line
verbatim (`examples/bringup/board-selftest/README.md` — cited as a path
rather than a link on purpose, same reason as §0: Doxygen's `INPUT` scans
only the top-level `examples/README.md`, not every example's own README, so a
markdown link here becomes an unresolvable `\ref` and fails the build).
A `FAIL (ALP_ERR_NOT_PROVISIONED)` there means the module was never run
through `scripts/program_eeprom.py` — fix provisioning before benching, do not
guess a revision. Family-specific equivalents that dump the raw 128-byte
manifest and every decoded field exist too, and need no `som.sku` edit — each
ships already pinned to its own family's SKU:
`examples/aen/aen-eeprom-manifest` (`som.sku: E1M-AEN801`; bench-verified) and
`examples/v2n/v2n-eeprom-manifest-dump` (`som.sku: E1M-V2N101`) — use either
if you want the full manifest, not just the summary line.
There is no separate HOST-side reader today: the manifest is read by the
module's own firmware over I2C
([`alp_hw_info_read()`](../../include/alp/hw_info.h),
[`docs/board-id.md`](../board-id.md) "Runtime read flow"), so getting
`hw_rev` costs one flash+boot, not a debugger-probe register peek.

**Why `hw_rev`, `core` and the profile are in the filename and not only in the
body.** Each of them changes the number, so two measurements that differ in one
of them are two measurements — and if the filename does not carry them, both
resolve to one path and the second silently destroys the first. The survivor is
then exactly measured and describes a different machine, which is §0(a)'s
hazard applied to the whole point rather than to one field: nothing about it
looks wrong.

**`model.source` is required.** A `sha256` is a well-formed 64-hex string
whatever model it came from, so a hash with no provenance would pass every
structural check while serving another model's latency under
`basis: "bench"` / `confidence: "certain"`. If the bytes ship in-tree, give the
repo-relative path and the suite re-hashes them for you. If they are
licence-gated or out of tree — most of the zoo — cite them
(`alp-sdk-internal:models/person_detect_int8.tflite`, or a URL). What is
refused either way is a path on **your** disk: a leading `/`, a `C:\` drive
prefix, a `OneDrive` segment, or a `..` that climbs out of the checkout.

---

## 2. Compile, and record the compile

### Ethos-U (`vela`)

The invocation, exactly as
[`examples/aen/aen-npu-inference-alp/gen_model.py`](../../examples/aen/aen-npu-inference-alp/gen_model.py)
builds it:

```
vela --accelerator-config <accel_config> \
     --config <ensemble_vela.ini> \
     --system-config <system_config> \
     --memory-mode <memory_mode> \
     --output-dir <out_dir> \
     <model.tflite>
```

* `--config` / `--system-config` / `--memory-mode` are **all three or none**.
  The Alif `.ini` is proprietary and comes from `alp-sdk-internal`; the SoC
  spec's `npu_toolchain.vela` block
  (`metadata/socs/alif/ensemble/e8.json` declares `memory_mode: "Sram_Only"`,
  `system_config_requires_vendor_config: true`,
  `vendor_config_filename: "ensemble_vela.ini"`) says which names apply.
* **Never pass a profile you guessed.** A wrong one compiles a command stream
  for memory the module does not have.
* Record the profile vela ITSELF reported for the run — it prints the
  `System config` / `Memory mode` it resolved in its own network-summary block.
  That is what goes in `toolchain.system_config` / `toolchain.memory_mode`, not
  what you intended to pass.
* `vela` exits **0 whether it placed every operator on the NPU or none of
  them**. Its exit code is not a verdict. Read the "CPU operators = N (P%) /
  NPU operators = N (P%)" lines for `measured.npu_ops` / `measured.cpu_ops`,
  and the per-run summary CSV (`<stem>_summary_<system_config>.csv`, one
  `<area>_memory_used` column per memory area) for the footprint.
* Give each run its **own** `--output-dir`. A shared directory means one run's
  summary is read for another's.

### DEEPX DX-M1 (`dxcom`) and DRP-AI

`dxcom -m <model.onnx> -c <config.json> -o <out_dir>` (ONNX in, licence-gated
wheel, single `.dxnn` out). DRP-AI goes through the DRP-AI Translator / TVM
path. Both publish far less about placement than vela does: record what the
tool actually prints, and **omit** any field it does not report. Do not
back-fill an op count from the model graph — that is an estimate.

Record every version pin the result depends on that alp-sdk does not override
(DRP-AI's `drp_compiler_version`, for instance) under `toolchain.pins`. A point
is only evidence for a compile that used those same pins.

---

## 3. Flash and run

Do **not** re-derive the flashing procedure here; it is already written down
per SoM, is bench-proven, and drifts if it is copied:

| SoM family | Procedure |
| --- | --- |
| E1M-AEN (Alif Ensemble) | [`docs/aen-bench-bringup.md`](../aen-bench-bringup.md) §2 — Flow A (SETOOLS MRAM), Flow C (J-Link RAM-run), Flow D (J-Link MRAM). Debugger attach: [`docs/debugging-aen.md`](../debugging-aen.md) |
| E1M-V2N / E1M-V2M | [`docs/bring-up-v2n.md`](../bring-up-v2n.md), [`docs/bring-up-v2n-m1.md`](../bring-up-v2n-m1.md); DRP-AI specifics in [`docs/bring-up-drpai-v2n.md`](../bring-up-drpai-v2n.md) |
| E1M-NX9101 | [`docs/bring-up-imx93.md`](../bring-up-imx93.md) |

Agent-facing equivalents: the `flashing-and-bench-debugging-aen` and
`flashing-and-bench-debugging-v2n` skills. The bench is **serial** — it never
runs inside a parallel workflow.

Run the model through the portable surface (`alp_inference_open()` →
`alp_inference_invoke()` → `alp_inference_get_output()`), not a raw
vendor-driver path, so what you measure is what a customer gets.
`examples/aen/aen-npu-inference-alp` is that path on AEN.

> **Gap, stated precisely — there is a harness to lift, not a blank page.**
> No **AEN** example times an inference today: `aen-npu-inference-alp` runs
> `alp_inference_invoke()` once and prints `RESULT PASS`, and
> `<alp/inference.h>` exposes no latency accessor, so nothing in the portable
> API hands you a figure. But two camera-vision examples already bracket the
> invoke with the core's cycle counter and are the pattern to lift:
> [`examples/camera-vision/ai-object-detection-realtime/src/main.c`](../../examples/camera-vision/ai-object-detection-realtime/src/main.c)
> lines 227-232, and
> [`examples/camera-vision/ai-camera-viewer/src/inference_loop.c`](../../examples/camera-vision/ai-camera-viewer/src/inference_loop.c)
> lines 112-122. Both take `k_cycle_get_32()` either side of
> `alp_inference_invoke()` and convert with `k_cyc_to_us_floor32()`. Neither is
> a bench harness — they time one invoke per frame to drive an on-screen
> latency readout, with no warm-up discard, no run loop and no percentile — so
> the first campaign still owes §4's loop. It owes the loop, not the
> measurement primitive. Until that lands, a point may legitimately carry the
> compile-derived fields and **omit** latency entirely.

---

## 4. Measure latency — never a single shot

A single inference measures the cache state it happened to start in.

1. **Warm up**: run at least **10** inferences and discard them. First-run cost
   (cold I-cache, first NPU command-stream fetch, lazily-faulted arena pages) is
   real, but it is a different measurement and does not belong in a steady-state
   mean.
2. **Time at least 100 runs.** This is a policy floor, not a measured one: it is
   set so `latency_ms_p95` is the 95th percentile of at least a hundred samples
   rather than an interpolation across a handful. It is also the run count the
   tier-2 plan's own worked example uses. **`scripts/validate_metadata.py`
   enforces it** — a point with `runs: 1` and a mean and p95 that are the same
   single number is refused, not merely discouraged. The floor lives in the
   validator rather than as a `minimum` in the schema on purpose: it is bench
   policy rather than document structure, its refusal can name this section and
   the reason where a bare `minimum` could only say `1`, and the schema is the
   wire contract a consumer pins against — a consumer must accept any point
   alp-sdk published rather than re-derive our bench policy.
3. Time **`alp_inference_invoke()` only** — not tensor fill, not output copy,
   not printing. Use the highest-resolution clock the core offers and state
   which one in the capture.
4. Report **all three**: `latency_ms_mean`, `latency_ms_p95`, `runs`. The schema
   makes `runs` a hard dependency of either latency field, and
   `latency_ms_p95` a dependency of `latency_ms_mean`, precisely so a mean can
   never ship without the evidence of how it was obtained. A `p95` below the
   `mean` is refused by the metadata gate: that is two runs' figures pasted into
   one point.
5. Do not average across power states, DVFS points or thermal conditions
   silently. If the module was at a non-default clock or the run was thermally
   limited, that belongs in `notes` — or the point belongs recaptured.

---

## 5. Where the raw capture goes

**Public/private split, and it is not negotiable:** the *schema* and the *perf
points* are public. The *raw captures* are not.

* Raw serial logs, PSU traces, scope captures, per-run latency tables and any
  unreleased-silicon detail go to **`alp-sdk-internal`**, under
  `bench/captures/`.
* The public point **cites** its capture; it never embeds it:

  ```json
  "capture": {
    "date": "2026-08-16",
    "operator": "<who ran it>",
    "reference": "alp-sdk-internal:bench/captures/2026-08-16-aen801-person-detect.log"
  }
  ```

* `capture.reference` is a `<store>:<path-within-store>` citation, and the
  schema enforces that **shape** rather than blacklisting bad ones: `see the
  log`, `ask the operator` and `n/a` are all refused, because a denylist of
  three known-bad spellings accepts every non-citation anyone would actually
  type. On top of the shape, the metadata gate refuses a leading `/`, a `C:\`
  drive prefix (which otherwise reads as a store named `C`) or a `OneDrive`
  segment — those leak a developer's machine into a public repo and resolve for
  nobody else, which also ends the point's reproducibility.
* When in doubt about a file's side of the line, apply the repo's
  public-vs-internal rule (`classifying-public-vs-internal`) before committing,
  not after.

---

## 6. Write the point, then let the gate judge it

Path — every segment is re-derived from the body by the gate, so it cannot
drift:

```
metadata/model_perf/<sku>/<target>/<model-slug>-<sha256[0:12]>@<toolchain>-<version>+<hw_rev>+<core>+<profile12>.json
```

`<target>` is `accel_config` when the backend has one, and `backend` when it
does not. `<profile12>` is the first 12 hex characters of the sha256 of the
canonical JSON (sorted keys, no whitespace) of every key under `toolchain`
**other than** `name` and `version` — today `system_config`, `memory_mode` and
`pins`. You do not compute it by hand: the gate tells you the filename your body
implies, so write the file, run the gate, and rename to what it names. Example:

```
metadata/model_perf/E1M-AEN801/ethos-u85-256/person-detect-int8-808cfdfc0cf3@vela-5.1.0+r2+m55_hp+1e562a678c9f.json
```

Every segment after the slug is one that changes the number, and each is there
because leaving it out lets a second measurement overwrite a first that is still
correct: re-benching changed model bytes must **accumulate** a second point (the
first is still the right answer for a customer holding the old bytes), an r1 and
an r2 point are different modules, an `a32_cluster` and an `m55_he` point are
different processors, and an `Ethos_U85_SRAM_Only` and an
`Ethos_U85_SYS_DRAM_Mid` point are different memory systems.

Then:

```
python3 scripts/validate_metadata.py
python3 -m pytest tests/scripts/test_model_perf_metadata.py -q
```

A shape to copy from lives at
`tests/fixtures/model_perf/E1M-AEN801/ethos-u85-256/person-detect-int8-808cfdfc0cf3@vela-5.1.0+r2+m55_hp+1e562a678c9f.json`.
It is a **synthetic fixture**: it carries a `_fixture` banner, every value under
its `measured` block is a placeholder, and the gate refuses that key anywhere
under `metadata/model_perf/`, so it cannot be promoted into the published tree
by accident. Copy its structure; copy none of its numbers.

---

## 7. When a figure cannot be measured

**Omit the field. Never estimate it.** There is no estimated perf point — that
is the static screen's job (tier 1), and a customer cannot tell the two apart
once they are in the same file. Every key under `measured` is optional for
exactly this reason, and a present key is a promise that a real run produced it.

| Situation | Do this |
| --- | --- |
| The toolchain does not report op placement | omit `npu_ops` / `cpu_ops` |
| No timing harness on this core yet (§3) | omit `latency_ms_mean`, `latency_ms_p95`, `runs` |
| The const-region question (§0b) is still open for this part | omit `req_sram_kib`; keep `arena_bytes` if the toolchain reported it |
| A figure came out zero | ship the zero **only if it was measured**. A zero that means "the tool told us nothing" is an omission, not a zero |
| The whole run could not be completed | **do not create the file.** A partial point still reads as authoritative |

**`req_sram_kib` is the one figure where a wrong zero is not merely wrong, and
the gate now enforces it rather than warning about it.** The on-device
selector's fit test is
`e->arena_sram_kib == 0u || t->req_sram_kib <= e->arena_sram_kib`
(`src/backends/inference/alp_model_select.c`), so a `req_sram_kib` of 0 — or any
figure below the arena the same compile reported — fits every arena on every
engine and turns the fit gate into a check that cannot fail. Whenever both are
present, `scripts/validate_metadata.py` requires
`req_sram_kib * 1024 >= arena_bytes`. Omit the field; never zero-fill it.

Whatever you could not measure, and why, goes in `notes`. That text is what
tells the next campaign what is still owed — and it is the difference between a
known gap and a silent one.

---

## 8. What a consumer does with the point

`tan model check` resolves in tiers: **precomputed → exact-if-toolchain →
static**. A matched point is reported as `basis: "bench"` with
`confidence: "certain"` and the `capture.reference` alongside, so the number
stays traceable to the run that produced it.

A match requires **exact** agreement on all eight of sku, hw_rev, core, backend,
accel_config, model `sha256`, toolchain name and toolchain version — a
consumer's match rule keys on everything that changes the number. There is no
"closest model", no "same model, different toolchain version" and no "same
module, other revision": a near miss is not a match, it falls through to the
next tier.

The toolchain **profile** is part of the file identity but deliberately *not*
part of that match key, because a customer holding no toolchain cannot state a
profile. So a consumer whose match key leaves **more than one** point standing
must not pick one arbitrarily — those points were measured on machines that
differ. It either surfaces all of them or falls through; the natural tiebreak is
the profile the SoC spec's own `npu_toolchain` block declares for the part.

**Absence is `undetermined`, never a negative.** Most model/SKU/target
combinations have never been benched. A consumer that reads a missing point as
"does not fit" reports a false no-fit on nearly everything we sell.
