# community.alplab.ai — content sync (scope A)

**Date:** 2026-05-14
**Target:** Discourse forum at `community.alplab.ai` running on
Hostinger VPS (`erp.alplab.ai`, `31.97.73.18`), container `app`
based on `local_discourse/app` standalone install.
**Source repos:**
[`alp-sdk`](https://github.com/alplabai/alp-sdk),
[`alp-sdk-vscode`](https://github.com/alplabai/alp-sdk-vscode),
[`alp-zephyr-modules`](https://github.com/alplabai/alp-zephyr-modules),
[`e1m-spec`](https://github.com/alplabai/e1m-spec).

## Goal

Bring the forum's visible content in line with the current state of
the four upstream repos so a first-time visitor lands on something
that reflects 2026-05-14 reality — not the 2026-04-04 seed.

Out of scope (deferred to a later doc): rotating the leaked SMTP
secret in `app.yml`, restoring the Discourse footer attribution,
GitHub-OAuth contributor linking, theming, automation.

## Forum state before this change

- 4 real users, 37 topics, 51 posts, 0 tags.
- 16 categories already structured (3 trees: `E1M Hardware/*`,
  `Alp SDK/*`, plus flat top-level).
- Most existing topics were seeded on 2026-04-04 and have no tags;
  three duplicate "Welcome to Alp Lab Community!" topics exist
  (2026-04-26, 2026-05-09, 2026-05-14).
- ~45 plugins enabled, none configured beyond defaults.

## Changes

1. **Four announcement topics under `Announcements` (id 7)**, one
   per repo, each with the current status pulled from that repo's
   `README.md` / `CHANGELOG.md` / `VERSIONS.md`:
   - `alp-sdk` v0.3 candidate progress (2026-05-14 highlights:
     hal_alif promoted to first-class upstream, DEEPX rail-mgmt,
     SLSA L2→L3, doxygen pass, vendor-partnership verification
     matrix for Alif / NXP / Renesas).
   - `e1m-spec` v1.1.1 release (2026-05-10).
   - `alp-sdk-vscode` first cut (LSP-native `board.yaml` editing,
     loader commands, west wrappers, debug orchestration).
   - `alp-zephyr-modules` scaffold published (board files pending
     hardware bring-up; declared as a no-op Zephyr module so
     consumers don't 404).

2. **Pinned getting-started index topics**, one per relevant
   category, each linking to the canonical repo doc(s):
   - `Alp SDK > Getting Started` (id 15): link the SDK's
     `docs/firmware-quickstart.md`, `docs/getting-started.md`,
     `docs/glossary.md`, `docs/troubleshooting.md`.
   - `E1M Hardware > E1M-AEN` (id 11): link the AEN one-pager,
     bring-up doc, and example list.
   - `E1M Hardware > E1M-X V2N` (id 12): link the V2N one-pager,
     bring-up-v2n, and V2N-specific examples.
   - `E1M Hardware > E1M-X V2N-M1` (id 13): link the V2N-M1
     one-pager and DEEPX bring-up delta.

3. **Tag taxonomy created and applied** to existing topics:
   - Surface tags: `zephyr`, `bare-metal`, `yocto`.
   - SoM tags: `aen`, `v2n`, `v2n-m1`, `imx93`.
   - Subsystem tags: `deepx`, `board-yaml`, `vscode`, `e1m-spec`,
     `mproc`, `security`, `dsp`, `camera`, `audio`.
   - Plus `getting-started` and `release` as cross-cutting markers.
   - Apply to the 37 existing topics where the title/category
     makes the tag self-evident; skip the rest.

4. **Category descriptions** populated for the eight that are
   currently empty, each ~1 sentence and pointing at the
   responsible repo + the matching doc index.

5. **RSS polling** enabled via `discourse-rss-polling` (already
   installed) for the four repos' GitHub release feeds, posting
   into `Announcements` with author = `system`. One polling row
   per repo, 30-minute interval, embed full release body.

## Order of operations

1. Take a fresh Discourse backup (`discourse-doctor`/`launcher
   backup`) before any writes, store on the VPS at
   `/root/alp-lab-community-pre-content-sync-<date>.tar.gz`.
2. Run all changes as a single Rails-runner script copied into
   the container, so they're idempotent and revertable as a
   transactional batch.
3. After execution, dump category descriptions + topic counts +
   tag list and post the diff in the chat for verification.

## Risk and rollback

- **Risk**: Topics created with the wrong category id, tags
  created with the wrong name, RSS polling spamming the
  Announcements category if the embed pulls historical releases.
- **Mitigation**: Pre-flight the Rails script by running it with
  a `DRY_RUN=1` env switch that logs intended writes without
  executing them. Verify the printed plan before flipping the
  switch.
- **Rollback**: Restore the pre-change backup snapshot; or for
  partial rollback delete the four new announcement topics and
  unset the four new RSS-polling rows from the admin UI.

## Done when

- The four announcement topics exist in `Announcements` and each
  has a non-zero post body that matches the repo's current
  `CHANGELOG.md`/`README.md` claim.
- Each of the four target categories has its pinned getting-started
  topic, and the category description is filled.
- The tag taxonomy is present and the seed topics carry their
  obvious tags.
- The RSS polling job has run at least once and the polling table
  shows `last_polled_at` for each row.
- A short post in this chat lists "what changed" with category
  id, topic id, and url for each new item.
