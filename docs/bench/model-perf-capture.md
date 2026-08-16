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
Until a module vela profile is resolved, every Ethos-U point would be captured
under vela's **built-in default** (`Ethos_U85_SYS_DRAM_Mid` /
`Dedicated_Sram_384KB`) — a DRAM-backed profile on a part with no DRAM. A
tier-2 point captured that way *is exactly measured* and *describes the wrong
machine*, which is the most dangerous shape a measurement can take: nothing
about it looks wrong. Alif's own sections (`Ethos_U85_SRAM_Only` and its
per-core siblings) live only in the proprietary `ensemble_vela.ini`, which
alp-sdk does not redistribute, and handing vela a section it cannot resolve is
a hard `rc=1`, not a degradation.

*Consequence for this recipe:* an `ethos_u` point **must** record both
`toolchain.system_config` and `toolchain.memory_mode`.
`scripts/validate_metadata.py` refuses one that does not — a point that omits
the profile cannot be told apart from a point captured under the default.

**(b) The const-region question — what does `req_sram_kib` actually count?**
`req_sram_kib` today reports the **arena alone** (the plan records 72 KiB
measured for `person_detect_int8.tflite`) while roughly 307 KiB is genuinely
SRAM0-resident. A bench point should record what is REALLY resident, and that
needs the per-part answer. Until it exists: **omit `req_sram_kib`** rather than
publish the arena under a name that promises more, and say so in `notes`.

---

## 1. Fix the identity before you measure anything

A perf point is pinned to what produced it, or it is a lie. Write these six
down first; every one of them ends up in the file **and** in its path, and a
consumer matches on all of them exactly:

| Identity field | Where it comes from |
| --- | --- |
| `measured_on.sku` | the module in the fixture, e.g. `E1M-AEN801`; must exist as `metadata/e1m_modules/<sku>.yaml` |
| `measured_on.hw_rev` | the module's **revision key** (`r1`, `r2`, …) from `metadata/e1m_modules/<family-dir>/hw-revisions.yaml` — not the Altium `board_rev` string |
| `measured_on.backend` | `ethos_u` \| `drpai` \| `deepx_dxm1` \| `cpu` |
| `measured_on.accel_config` | e.g. `ethos-u85-256`; `""` for a backend with no such knob |
| `model.sha256` | `sha256sum` of the **exact bytes you compile**, before any toolchain touches them |
| `toolchain.name` + `.version` | the compiler you ran, and the version it reports |

`(backend, accel_config)` must be one the SKU actually resolves — derived from
the host SoC's `npus[]` plus any on-module discrete accelerator SoC whose
`variants[].alp_module_skus` lists the SKU. The metadata gate refuses a pair
the module does not have.

If the module in front of you is not at a revision the family table knows,
**stop**: add the revision to `hw-revisions.yaml` first. A point tagged with
the wrong revision is worse than no point.

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

> **Gap, stated rather than glossed:** no example in this repo times an
> inference today. `aen-npu-inference-alp` runs `alp_inference_invoke()` once
> and prints `RESULT PASS`; `<alp/inference.h>` exposes no latency accessor.
> The first campaign therefore has to add a timing harness (§4) before it can
> fill in any `latency_*` field. Until that lands, a point may legitimately
> carry the compile-derived fields and **omit** latency entirely.

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
   tier-2 plan's own worked example uses.
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

* `capture.reference` is a `<store>:<path-within-store>` citation. The metadata
  gate refuses a leading `/`, a `C:\` drive prefix or a `OneDrive` segment —
  those leak a developer's machine into a public repo and resolve for nobody
  else, which also ends the point's reproducibility.
* When in doubt about a file's side of the line, apply the repo's
  public-vs-internal rule (`classifying-public-vs-internal`) before committing,
  not after.

---

## 6. Write the point, then let the gate judge it

Path — every segment is re-derived from the body by the gate, so it cannot
drift:

```
metadata/model_perf/<sku>/<target>/<model-slug>-<sha256[0:12]>@<toolchain>-<version>.json
```

`<target>` is `accel_config` when the backend has one, and `backend` when it
does not. Example:

```
metadata/model_perf/E1M-AEN801/ethos-u85-256/person-detect-int8-808cfdfc0cf3@vela-5.1.0.json
```

The model hash is in the filename deliberately: re-benching a model whose bytes
changed must **accumulate** a second point, never overwrite the first — the
first is still the correct answer for a customer holding the old bytes.

Then:

```
python3 scripts/validate_metadata.py
python3 -m pytest tests/scripts/test_model_perf_metadata.py -q
```

A shape to copy from lives at
`tests/fixtures/model_perf/E1M-AEN801/ethos-u85-256/person-detect-int8-808cfdfc0cf3@vela-5.1.0.json`.
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
| A figure came out zero | ship the zero **only if it was measured**. A zero that means "the tool told us nothing" is an omission, not a zero — a zero `req_sram_kib` satisfies the on-device fit gate against any arena |
| The whole run could not be completed | **do not create the file.** A partial point still reads as authoritative |

Whatever you could not measure, and why, goes in `notes`. That text is what
tells the next campaign what is still owed — and it is the difference between a
known gap and a silent one.

---

## 8. What a consumer does with the point

`tan model check` resolves in tiers: **precomputed → exact-if-toolchain →
static**. A matched point is reported as `basis: "bench"` with
`confidence: "certain"` and the `capture.reference` alongside, so the number
stays traceable to the run that produced it.

A match requires **exact** agreement on all of sku, backend, accel_config,
model `sha256`, toolchain name and toolchain version. There is no "closest
model" and no "same model, different toolchain version" — a near miss is not a
match, it falls through to the next tier.

**Absence is `undetermined`, never a negative.** Most model/SKU/target
combinations have never been benched. A consumer that reads a missing point as
"does not fit" reports a false no-fit on nearly everything we sell.
