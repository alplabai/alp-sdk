#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Alarm when a PR is silently ejected from the `dev` merge queue (issue #1952).

Background: GitHub's merge queue drops an entry whose checks have not
*reported* within `check_response_timeout_minutes` -- even when none of them
failed. The ejected PR keeps reading OPEN / MERGEABLE / CLEAN with every one
of its own checks green, so nothing in the PR view says it was dropped; the
only visible symptom is that `dev` stops moving while the queue looks
healthy. This script makes that silent state visible. It is run on a
schedule by .github/workflows/merge-queue-ejection-alarm.yml -- it is an
alarm, not a gate: it never fails the workflow on a detection, only when the
GitHub API itself errors.

Detection (matches issue #1952's own description of the observable state):

  1. PR targets `dev`, is open, not a draft, `mergeable == MERGEABLE` and
     `mergeStateStatus == CLEAN`. CLEAN already means "every required check
     is passing and nothing blocks an immediate merge" -- GitHub computes
     it, this script does not re-derive it from individual check runs.
  2. PR is NOT in the merge queue right now (live `mergeQueue { entries }`
     GraphQL query on the repo).
  3. The PR's timeline shows it WAS enqueued and the *last* queue-related
     event is `removed_from_merge_queue` (no later `added_to_merge_queue`),
     and that removal happened more than EJECTION_DWELL_MINUTES ago.

Condition 3 is what keeps this from alarming on the common, correct reasons
a PR sits green and unqueued: nobody asked the queue to build it yet, it is
a draft, it is waiting on review, or it is deliberately held (this repo
parks bench-owed work as `needs-silicon` and leaves it open on purpose) --
none of those PRs have a `removed_from_merge_queue` event at all, so step 3
excludes them for free without needing to know about labels.

A `removed_from_merge_queue` event's actor distinguishes the two ways an
entry leaves the queue: the bot app `github-merge-queue[bot]` performs the
removal when the queue itself ejects an entry (timeout) or completes a
merge, while a human clicking "Remove from queue" is recorded with *that
person's own login* as actor. Confirmed against this repo's live data: PR
#1887's three removals (the recorded #1952 incident) all show actor
`github-merge-queue[bot]`; PRs #1673 and #1670 (`needs-silicon`, held on
purpose) were removed with actor `alpCaner` -- a human dequeuing them during
the #1952 mitigation, not an ejection. So this script only treats a removal
as a candidate ejection when its actor is the bot; a human-removed PR is
correctly ignored no matter how green or how long it has sat. The merge
case (bot removal immediately followed by an actual merge) is excluded for
free anyway: a merged PR's state is MERGED, not OPEN, so it never reaches
this script's candidate list at all.

Alarms ONCE per ejection: it posts a PR comment carrying a hidden marker
with the timeline event's unique id, and skips a PR that already carries a
comment with that marker for that event.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from datetime import datetime, timezone

REPO = os.environ.get("GITHUB_REPOSITORY", "alplabai/alp-sdk")
BASE_BRANCH = "dev"
EJECTION_DWELL_MINUTES = 15
MERGE_QUEUE_BOT_LOGIN = "github-merge-queue[bot]"
MARKER_TEMPLATE = "<!-- merge-queue-ejection-alarm:event-id={event_id} -->"

MERGE_QUEUE_QUERY = """
query($owner: String!, $name: String!) {
  repository(owner: $owner, name: $name) {
    mergeQueue {
      entries(first: 100) {
        nodes { pullRequest { number } }
      }
    }
  }
}
"""


def _gh(*args: str) -> str:
    """Run `gh <args>` and return stdout, or die with a clear error."""
    try:
        result = subprocess.run(
            ("gh", *args),
            check=True,
            capture_output=True,
        )
    except FileNotFoundError:
        sys.exit("error: GitHub CLI (`gh`) not found.")
    except subprocess.CalledProcessError as exc:
        stderr = exc.stderr.decode(errors="replace").strip()
        sys.exit(f"error: `gh {' '.join(args)}` failed:\n{stderr}")
    return result.stdout.decode()


def _gh_json(*args: str):
    return json.loads(_gh(*args))


def queued_pr_numbers() -> set[int]:
    """PR numbers currently sitting in the repo's merge queue, right now."""
    owner, name = REPO.split("/")
    data = _gh_json(
        "api", "graphql",
        "-f", f"query={MERGE_QUEUE_QUERY}",
        "-F", f"owner={owner}",
        "-F", f"name={name}",
    )
    merge_queue = data["data"]["repository"]["mergeQueue"]
    if merge_queue is None:
        return set()
    return {node["pullRequest"]["number"] for node in merge_queue["entries"]["nodes"]}


def candidate_prs() -> list[dict]:
    """Open, non-draft PRs targeting BASE_BRANCH that are green and ready."""
    prs = _gh_json(
        "pr", "list",
        "--repo", REPO,
        "--base", BASE_BRANCH,
        "--state", "open",
        "--json", "number,title,url,isDraft,mergeable,mergeStateStatus",
        "--limit", "200",
    )
    return [
        pr for pr in prs
        if not pr["isDraft"]
        and pr["mergeable"] == "MERGEABLE"
        and pr["mergeStateStatus"] == "CLEAN"
    ]


def last_queue_removal(number: int) -> dict | None:
    """The PR's most recent removed_from_merge_queue event, iff it was the
    bot ejecting/completing the entry (not a human dequeuing it) and
    nothing queue-related happened after it. None if: the PR was never
    enqueued (no such event at all); an add-event is the more recent of the
    two (the live not-queued check above should already rule this out --
    treat it as "don't know yet" rather than alarm on a race); or the
    removal's actor is a human, meaning someone deliberately pulled it and
    this is not an ejection."""
    events = _gh_json("api", f"repos/{REPO}/issues/{number}/timeline", "--paginate")
    last_add = None
    last_remove = None
    for event in events:
        kind = event.get("event")
        if kind == "added_to_merge_queue":
            last_add = event
        elif kind == "removed_from_merge_queue":
            last_remove = event
    if last_remove is None:
        return None
    if last_add is not None and last_add["created_at"] > last_remove["created_at"]:
        return None
    actor = last_remove.get("actor") or {}
    if actor.get("login") != MERGE_QUEUE_BOT_LOGIN:
        return None
    return last_remove


def already_alarmed(number: int, event_id: int) -> bool:
    marker = MARKER_TEMPLATE.format(event_id=event_id)
    bodies = _gh_json(
        "api", f"repos/{REPO}/issues/{number}/comments",
        "--paginate", "--jq", "[.[] | .body]",
    )
    return any(marker in body for body in bodies)


def post_alarm(number: int, removed_at: str, event_id: int, dwell_minutes: float) -> None:
    marker = MARKER_TEMPLATE.format(event_id=event_id)
    body = (
        "**Merge queue ejection detected** (issue #1952)\n\n"
        f"This PR was removed from the `{BASE_BRANCH}` merge queue at "
        f"{removed_at} (~{dwell_minutes:.0f} minutes ago) and has not been "
        "re-added since. It is currently `MERGEABLE` / `CLEAN` with every "
        "required check passing -- nothing about the change failed. This "
        "matches the silent-ejection failure issue #1952 describes: the "
        "queue's `check_response_timeout_minutes` dropped the entry while "
        "its checks were still queued behind other runner load, not "
        "because anything is wrong with it.\n\n"
        "It will not merge on its own -- re-add it to the merge queue when "
        f"ready.\n\n{marker}"
    )
    _gh("api", f"repos/{REPO}/issues/{number}/comments", "-f", f"body={body}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--dry-run", action="store_true",
        help="Detect and print, but do not post PR comments.",
    )
    args = parser.parse_args()

    queued = queued_pr_numbers()
    candidates = [pr for pr in candidate_prs() if pr["number"] not in queued]

    now = datetime.now(timezone.utc)
    fired = 0
    for pr in candidates:
        removal = last_queue_removal(pr["number"])
        if removal is None:
            continue
        removed_at = datetime.strptime(
            removal["created_at"], "%Y-%m-%dT%H:%M:%SZ"
        ).replace(tzinfo=timezone.utc)
        dwell_minutes = (now - removed_at).total_seconds() / 60
        if dwell_minutes < EJECTION_DWELL_MINUTES:
            continue
        if already_alarmed(pr["number"], removal["id"]):
            continue
        print(
            f"EJECTED: PR #{pr['number']} removed at {removal['created_at']} "
            f"(~{dwell_minutes:.0f} min ago) -- {pr['title']}"
        )
        fired += 1
        if not args.dry_run:
            post_alarm(pr["number"], removal["created_at"], removal["id"], dwell_minutes)

    if fired == 0:
        print("No merge-queue ejections detected.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
