# 3rd-Party Library Examples Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship one focused, `native_sim`-runnable teaching example for each of the 15 currently-undemonstrated third-party libraries, fixing any build-blocking compatibility gap along the way.

**Architecture:** Each example follows the repo's single-OS example anatomy (`board.yaml` + `prj.conf` + `CMakeLists.txt` + `src/main.c(pp)` + `README.md` + `testcase.yaml`). It exercises the library's core logic in a CPU/RAM-backed way that builds AND runs on `native_sim/native/64` (no hardware), printing a deterministic final marker line the twister `console`/`one_line` harness asserts. Shared-file compatibility fixes (west pins, Kconfig knobs, the `gfx_compat` in-tree shim) are done once, serially, before the examples are authored in parallel.

**Tech Stack:** Zephyr v4.4.0, west, `scripts/alp_project.py` (board.yaml → alp.conf), twister (`native_sim`), clang-format-22, the `metadata/library-profiles/<lib>/` profile mechanism.

## Global Constraints

- **Base branch:** `feat/3p-library-examples` off `origin/dev`; PR targets `dev`, never `main`.
- **Build depth:** every example is `native_sim/native/64` build-and-run only; mark `[UNTESTED]` (no bench) in each README.
- **No wrappers:** examples use the library's native API directly (`#include "jsmn.h"`, `fmt::format_to(...)`, etc.). Do NOT add `<alp/*>` wrappers.
- **Teaching density:** each `src/main.c(pp)` carries ~50% comments per `writing-self-teaching-comments`; examples are documentation.
- **House style:** clang-format-22, tabs, per `applying-the-alp-sdk-c-house-style`. Run the formatter before every commit.
- **SDK invariants:** no heap on the hot path, no exceptions on M-class, no `<iostream>`. The library-profile header enforces these; the example must not undo them.
- **Attribution:** no "Co-Authored-By: Claude" / "Generated with Claude Code" in any commit or file.
- **Company/contact:** "Alp Lab" (exact caps); `contact@alplab.ai` if a contact is ever needed.
- **Final marker convention:** each runtime example ends `main` by printing `"[<name>] done"` (e.g. `[jsmn] done`); the `testcase.yaml` `one_line` regex asserts exactly that.
- **Copy the skeleton:** `board.yaml`, `CMakeLists.txt`, `prj.conf`, `testcase.yaml` are copied from `examples/peripheral-io/hello-world/` and adapted — do not hand-invent the CMake/alp_project.py plumbing.
- **Shared files are serial:** `west.yml`, `zephyr/Kconfig.alp-libraries`, `examples/README.md`, `metadata/catalog.json` are edited only in Task 0 / Task 16 — never by a parallel example task.

---

## Task 0: Compatibility fixes (serial, shared files) — do first

**Files:**
- Modify: `west.yml`
- Modify: `zephyr/Kconfig.alp-libraries`
- Create: `src/lib/gfx_compat/` (in-tree SW-blit shim + CMakeLists.txt + Kconfig)
- Create/Modify: `metadata/library-profiles/<lib>/` profile headers where a default breaks the build
- Create (if vendoring chosen): `vendors/{etl,fmt,doctest}/` header drops + README

**Interfaces:**
- Produces: a tree where `#include` for all 15 libraries resolves on a `native_sim` build, and the Kconfig knobs referenced by each example's `board.yaml` `libraries:` entry exist. Example tasks 1–15 consume this.

- [ ] **Step 1: Prove the current gap.** For each of etl, fmt, doctest, littlefs, gfx_compat, build a throwaway native_sim app that `#include`s the library and confirm it fails to resolve. Record the exact error per lib.

  Run (per lib, from the worktree): create `/tmp/probe-<lib>/` with a `board.yaml` (copy hello-world's) that lists the lib under `cores.m55_hp.libraries:`, a `main` that includes the header, then
  `west twister -T /tmp/probe-<lib> -p native_sim/native/64 --build-only -v`
  Expected: FAIL (header not found / Kconfig symbol missing) — this is the gap to close.

- [ ] **Step 2: etl / fmt / doctest fetch.** These are header-only C++ libraries with a profile header already present but no west pin. Add each as a west project under a new `extras-cpp` group in `west.yml` (mirror the `extras-tier1` block: remote + repo-path + revision + `path: modules/lib/<lib>` + `groups: [extras-cpp]`). Pins: `etl` → `github.com/ETLCPP/etl` tag `20.39.4`; `fmt` → `github.com/fmtlib/fmt` tag `11.0.2`; `doctest` → `github.com/doctest/doctest` tag `v2.4.11`. Add the three remotes. If a west pin proves heavier than a vendored header drop at build time, instead vendor the single public header under `vendors/<lib>/include/` with a README classified per `classifying-public-vs-internal` — pick one mechanism per lib and note which in the lib's example README.

- [ ] **Step 3: Wire the C++ profile headers onto the include path.** Confirm the `§D.lib.loader` in `scripts/alp_project.py` already adds `metadata/library-profiles/<lib>/` to the include path when the lib is in `libraries:` (the profile README documents this). If it does not for etl/fmt/doctest, add the emit so `etl_profile.h` / `fmt_config.h` / `doctest_config.h` win over upstream defaults. Verify by grepping the generated `alp.conf`/CMake for the include dir.

- [ ] **Step 4: gfx_compat in-tree shim.** `west.yml` states gfx_compat ships at `src/lib/gfx_compat/`, but `src/lib/` is empty. Create the minimal shim: `src/lib/gfx_compat/gfx_compat.c` + `include/gfx_compat.h` implementing a pure-C SW-blit fill/blit under `CONFIG_ALP_GFX_COMPAT_SW`, plus `CMakeLists.txt` + `Kconfig` hooked so `libraries: [gfx_compat]` compiles it. Keep the API tiny: `gfx_compat_fill(buf, w, h, color)` and `gfx_compat_blit(dst, src, w, h)`.

- [ ] **Step 5: littlefs.** Confirm the Zephyr `fs`/littlefs subsystem resolves on native_sim with a RAM/flash-sim backend. If the `ALP_LITTLEFS_*` knobs need a Kconfig `select` of `CONFIG_FILE_SYSTEM_LITTLEFS`, add it to `zephyr/Kconfig.alp-libraries` (or the loader) so `libraries: [littlefs]` pulls the subsystem in.

- [ ] **Step 6: Add any missing profile header** for a lib in tasks 1–15 whose upstream default breaks the SDK build (discovered when that example first fails to build). Add ONLY where needed; do not add a no-op header.

- [ ] **Step 7: clang-format + commit.**

```bash
cd /tmp/alp-3p-lib-examples
clang-format-22 -i src/lib/gfx_compat/*.c src/lib/gfx_compat/include/*.h
git add west.yml zephyr/Kconfig.alp-libraries src/lib/gfx_compat metadata/library-profiles vendors 2>/dev/null
git commit -m "feat(examples): wire etl/fmt/doctest/gfx_compat/littlefs for library examples"
```

---

## Tasks 1–15: one example each

**Shared per-task shape (applies to every example task below):**

**Files (per example, `<cat>` and `<name>` per the table):**
- Create: `examples/<cat>/<name>/board.yaml` (copy hello-world; set `cores.<coreid>.libraries: [<lib>]`; keep `som.sku`/`preset` as hello-world)
- Create: `examples/<cat>/<name>/prj.conf` (empty, copy hello-world)
- Create: `examples/<cat>/<name>/CMakeLists.txt` (copy hello-world; set `project(<name> LANGUAGES C)` — use `LANGUAGES C CXX` for C++ examples; `target_sources(app PRIVATE src/main.<c|cpp>)`)
- Create: `examples/<cat>/<name>/src/main.c(pp)`
- Create: `examples/<cat>/<name>/README.md` (what it shows + `[UNTESTED]` + the board.yaml HW-swap note)
- Create: `examples/<cat>/<name>/testcase.yaml` (copy hello-world; one `native_sim` + one `native_sim/native/64` scenario; `harness: console`, `one_line` regex `"\\[<name>\\] done"`; tags `alp-sdk example <name>`)

**Shared per-task steps:**
- [ ] Copy the hello-world skeleton into the new dir and adapt `board.yaml`/`CMakeLists.txt`/`testcase.yaml`.
- [ ] Write `src/main.c(pp)` per the task's "Behavior" (below), ending with `printf("[<name>] done\n");` (C) or the C++ equivalent.
- [ ] Write `README.md`.
- [ ] Build+run: `west twister -T examples/<cat>/<name> -p native_sim/native/64 -v` → Expected: PASS, console shows `[<name>] done`.
- [ ] `clang-format-22 -i` the source; `git add examples/<cat>/<name>` and commit `feat(examples): <lib> usage example`.

### Task 1: jsmn — `examples/connectivity/jsmn-json-parse` (C)
**Behavior:** embed a JSON config string; `jsmn_init` + `jsmn_parse` into a token array; walk tokens to extract two fields (a string + an int) into a struct; print the extracted values; assert token count > 0; print `[jsmn-json-parse] done`.

### Task 2: nanopb — `examples/connectivity/nanopb-encode-decode` (C)
**Behavior:** define a tiny `.proto` (message with an int32 + a string) and its generated `.pb.h/.pb.c` (or a hand-written `pb_msgdesc`); `pb_encode` into a byte buffer, `pb_decode` it back, assert fields round-trip; print decoded values; `[nanopb-encode-decode] done`. Wire nanopb generation in CMake per the Zephyr nanopb module (`CONFIG_NANOPB=y` via `libraries: [nanopb]`).

### Task 3: coremqtt_sn — `examples/connectivity/mqtt-sn-publish` (C)
**Behavior:** construct an MQTT-SN PUBLISH packet via the coreMQTT-SN serializer into a buffer, then deserialize it back and assert topic-id + payload match; no live transport (buffer round-trip); `[mqtt-sn-publish] done`.

### Task 4: libcoap — `examples/connectivity/coap-client-get` (C)
**Behavior:** build a CoAP GET request PDU (`coap_pdu_init` + options + token) into a buffer; parse a canned CoAP 2.05 Content response PDU and print its payload; no socket; `[coap-client-get] done`.

### Task 5: libwebsockets — `examples/connectivity/websocket-frame` (C)
**Behavior:** demonstrate WS framing offline — mask+encode a text frame into a buffer using the lws frame helpers (or the documented framing path), decode it back, assert payload matches; `[websocket-frame] done`. If lws cannot frame without a full `lws_context`, use the smallest context that inits on native_sim and note the limitation in the README.

### Task 6: tinygsm — `examples/connectivity/tinygsm-modem-at` (C++)
**Behavior:** drive `TinyGsm` against a mock Stream that replays a canned modem transcript (e.g. `AT`→`OK`, `AT+CGMI`→`SIMCOM`); call `modem.init()`/`getModemInfo()` and print results; `[tinygsm-modem-at] done`. The mock Stream stands in for the UART on native_sim; README documents the real-UART board.yaml swap.

### Task 7: minimp3 — `examples/audio/minimp3-decode` (C)
**Behavior:** embed a short MP3 frame as a byte array; `mp3dec_init` + `mp3dec_decode_frame` to PCM; compute + print sample count and RMS; assert samples > 0; `[minimp3-decode] done`.

### Task 8: libhelix — `examples/audio/libhelix-decode` (C)
**Behavior:** embed a short MP3 frame; init the Helix MP3 decoder, decode one frame to PCM, print sample count; assert > 0; `[libhelix-decode] done`. (If the vendored Helix flavor is AAC not MP3, decode a canned AAC frame instead and say so in the README.)

### Task 9: u8g2 — `examples/display/u8g2-oled-draw` (C)
**Behavior:** set up a u8g2 instance on the built-in framebuffer/null device (no real panel); draw a string + a frame + a filled box; then dump the RAM buffer as ASCII art to the console so it's visible on native_sim; `[u8g2-oled-draw] done`. README documents the SSD1306-over-I2C/SPI board.yaml swap.

### Task 10: gfx_compat — `examples/display/gfx-compat-blit` (C)
**Behavior:** allocate a small RGB565 buffer; `gfx_compat_fill` it, `gfx_compat_blit` a sub-rect; print a checksum of the result; `[gfx-compat-blit] done`. Depends on the Task 0 shim.

### Task 11: littlefs — `examples/power-timing/littlefs-keyvalue` (C)
**Behavior:** mount a RAM/flash-sim littlefs partition; write a key/value file, read it back, list the directory; assert the read matches the write; `[littlefs-keyvalue] done`. Uses the native_sim flash backend.

### Task 12: etl — `examples/peripheral-io/etl-fixed-containers` (C++)
**Behavior:** with the SDK's no-STL profile, build an `etl::vector<int, 8>` and an `etl::map<...>`; push/lookup; print size + a value; demonstrate the fixed-capacity/no-heap property in comments; `[etl-fixed-containers] done`. Depends on Task 0.

### Task 13: fmt — `examples/peripheral-io/fmt-formatting` (C++)
**Behavior:** `fmt::format_to` into a fixed `char[64]` buffer (no allocation, no iostream) formatting an int + float + string; print the buffer; `[fmt-formatting] done`. Depends on Task 0.

### Task 14: catch2 — `examples/testing/catch2-selftest` (C++)
**Behavior:** a small Catch2 test binary with 2–3 `TEST_CASE`s over a trivial pure function (e.g. a `clamp()` helper). Builds + runs on the host; must exit 0. `testcase.yaml` uses `harness: console` asserting Catch2's `All tests passed` line (adjust regex to Catch2's summary). This is the new `examples/testing/` category.

### Task 15: doctest — `examples/testing/doctest-selftest` (C++)
**Behavior:** header-only doctest binary with 2–3 `TEST_CASE`s over the same trivial helper; builds + runs on host; exit 0. Regex asserts doctest's success summary. Depends on Task 0's doctest fetch.

---

## Task 16: Index + catalog (serial, shared files)

**Files:**
- Modify: `examples/README.md`
- Modify: `metadata/catalog.json` (regenerate)

**Interfaces:** Consumes tasks 1–15 (needs all example dirs present).

- [ ] **Step 1:** Add each new example row to the correct category table in `examples/README.md`; add a new "Testing / host" section for the catch2 + doctest examples.
- [ ] **Step 2:** Regenerate the catalog: `py -3.14 scripts/gen_catalog.py` (or the repo's canonical invocation) and confirm `metadata/catalog.json` now lists the 15 examples.
- [ ] **Step 3:** Run `pytest tests/.../test_gen_catalog.py` (the catalog gate) → Expected: PASS.
- [ ] **Step 4:** Commit `docs(examples): index the 15 new library examples + regen catalog`.

---

## Task 17: Local CI + review gate

**Interfaces:** Consumes tasks 0–16.

- [ ] **Step 1:** Full twister on the new examples: `west twister -T examples -p native_sim/native/64 --tag example -v` → Expected: all new scenarios PASS.
- [ ] **Step 2:** clang-format gate: run the repo's `clang-format` check over all new/changed sources → Expected: clean (no diff).
- [ ] **Step 3:** Run the `check_*.py` gates the `running-local-ci` skill lists → Expected: PASS.
- [ ] **Step 4:** Dispatch `alp-reviewer` over the branch diff (applies `reviewing-alp-changes`); fix findings most-severe first.
- [ ] **Step 5:** Open a PR against `dev` summarizing the 15 examples + the compatibility fixes; link this plan + the spec.

---

## Self-Review

- **Spec coverage:** all 15 libs → tasks 1–15; compatibility fixes (etl/fmt/doctest pins, gfx_compat shim, littlefs, profile headers) → Task 0; index+catalog → Task 16; native_sim build-only + `[UNTESTED]` + `dev` base + no-wrapper are in Global Constraints; `examples/testing/` category created in Task 14. Covered.
- **Placeholder scan:** each example task names its exact dir, library API calls, marker line, and verify command; open per-lib fallbacks (lws context, Helix MP3-vs-AAC, west-pin-vs-vendor) are explicit decisions, not TODOs.
- **Type consistency:** marker convention `[<name>] done` is uniform; `gfx_compat_fill`/`gfx_compat_blit` defined in Task 0 and consumed only in Task 10; `libraries: [<lib>]` knob names match `metadata/library-profiles/<lib>/` and `zephyr/Kconfig.alp-libraries`.
