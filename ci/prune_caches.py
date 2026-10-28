#!/usr/bin/env python3
"""Prune this repository's GitHub Actions caches so only valid, recent ones remain.

Only caches whose key starts with --prefix (default "ccache-") are ever touched; anything else in the
repository's cache is left alone. Rules, applied in this order, each deletion tagged with its reason:

  closed-ref   the cache belongs to a pull request that is no longer open, or to a branch that no longer
               exists (it can never be restored again by a run on that ref);
  stale        not used for --max-idle-days (default 7; GitHub evicts at 7 days idle, we do it first);
  superseded   per (ref, key-without-the-trailing-commit-sha) group keep only the newest --keep (default 2,
               the latest plus the previous one for a rollback), delete the older ones;
  over-quota   if what is left is still larger than --quota-gib (default 8 of the 10 GiB repository
               limit), delete the least recently used until it fits.

  python3 ci/prune_caches.py            # delete (needs `gh` + `actions: write`)
  python3 ci/prune_caches.py --dry-run  # print the plan only
  python3 ci/prune_caches.py --self-test
"""
import argparse
import datetime as dt
import json
import subprocess
import sys

GIB = 1024 ** 3


def parse_time(s):
    return dt.datetime.fromisoformat(s.replace("Z", "+00:00"))


def group_of(key):
    """`ccache-v1-gcc-release-x86_64-<toolchainhash>-<commitsha>` -> everything but the commit sha."""
    head, sep, _ = key.rpartition("-")
    return head if sep else key


def plan(caches, now, open_prs, live_branches, prefix="ccache-", max_idle_days=7, keep=2, quota_gib=8.0):
    """Return [(cache, reason)] to delete. Pure function: no I/O, unit-testable."""
    managed = [c for c in caches if c["key"].startswith(prefix)]
    doomed = {}  # id -> (cache, reason)

    def doom(c, why):
        doomed.setdefault(c["id"], (c, why))

    for c in managed:
        ref = c.get("ref", "")
        if ref.startswith("refs/pull/"):
            number = ref.split("/")[2]
            if number not in open_prs:
                doom(c, "closed-ref")
        elif ref.startswith("refs/heads/") and ref[len("refs/heads/"):] not in live_branches:
            doom(c, "closed-ref")

    for c in managed:
        if c["id"] in doomed:
            continue
        if (now - parse_time(c["lastAccessedAt"])).days >= max_idle_days:
            doom(c, "stale")

    groups = {}
    for c in managed:
        if c["id"] not in doomed:
            groups.setdefault((c.get("ref", ""), group_of(c["key"])), []).append(c)
    for members in groups.values():
        members.sort(key=lambda c: parse_time(c["createdAt"]), reverse=True)
        for c in members[keep:]:
            doom(c, "superseded")

    left = [c for c in managed if c["id"] not in doomed]
    total = sum(c["sizeInBytes"] for c in left)
    for c in sorted(left, key=lambda c: parse_time(c["lastAccessedAt"])):  # least recently used first
        if total <= quota_gib * GIB:
            break
        doom(c, "over-quota")
        total -= c["sizeInBytes"]
    return list(doomed.values())


def gh_json(*args):
    out = subprocess.run(["gh", *args], check=True, capture_output=True, text=True).stdout
    return json.loads(out or "[]")


def self_test():
    now = dt.datetime(2026, 11, 1, tzinfo=dt.timezone.utc)

    def mk(i, key, ref, created, accessed, size=1 * GIB // 10):
        return {"id": i, "key": key, "ref": ref, "createdAt": created, "lastAccessedAt": accessed,
                "sizeInBytes": size}

    g = "ccache-v1-gcc-release-x86_64-abc"
    caches = [
        mk(1, g + "-sha1", "refs/heads/master", "2026-10-30T00:00:00Z", "2026-10-31T00:00:00Z"),  # newest
        mk(2, g + "-sha2", "refs/heads/master", "2026-10-29T00:00:00Z", "2026-10-31T00:00:00Z"),  # previous
        mk(3, g + "-sha3", "refs/heads/master", "2026-10-28T00:00:00Z", "2026-10-31T00:00:00Z"),  # superseded
        mk(4, g + "-sha4", "refs/pull/9/merge", "2026-10-30T00:00:00Z", "2026-10-31T00:00:00Z"),  # PR 9 closed
        mk(5, g + "-sha5", "refs/pull/7/merge", "2026-10-30T00:00:00Z", "2026-10-31T00:00:00Z"),  # PR 7 open
        mk(6, "ccache-v1-old-x-sha6", "refs/heads/master", "2026-10-01T00:00:00Z", "2026-10-10T00:00:00Z"),  # stale
        mk(7, "unrelated-cache-key", "refs/pull/9/merge", "2026-01-01T00:00:00Z", "2026-01-01T00:00:00Z"),  # not ours
        mk(8, g + "-sha8", "refs/heads/gone", "2026-10-30T00:00:00Z", "2026-10-31T00:00:00Z"),  # branch deleted
    ]
    got = {c["id"]: why for c, why in plan(caches, now, open_prs={"7"}, live_branches={"master"})}
    want = {3: "superseded", 4: "closed-ref", 6: "stale", 8: "closed-ref"}
    assert got == want, f"rules: {got} != {want}"

    # grouping is per (ref, key-without-sha): a different scope on the same ref is its own group
    other = "ccache-v1-clang-release-x86_64-abc"
    two = [mk(10 + i, other + f"-s{i}", "refs/heads/master", f"2026-10-{20 + i}T00:00:00Z",
              "2026-10-31T00:00:00Z") for i in range(3)] + caches[:2]
    got = {c["id"] for c, _ in plan(two, now, set(), {"master"})}
    assert got == {10}, f"per-group keep: {got}"  # only the oldest clang entry exceeds keep=2

    # quota: 5 live 2 GiB entries, quota 8 GiB -> the least recently used one goes
    big = [mk(20 + i, f"ccache-v1-s{i}-x-sha", "refs/heads/master", "2026-10-30T00:00:00Z",
              f"2026-10-{27 + i}T00:00:00Z", 2 * GIB) for i in range(5)]
    got = [(c["id"], why) for c, why in plan(big, now, set(), {"master"})]
    assert got == [(20, "over-quota")], f"quota: {got}"

    assert plan([], now, set(), set()) == []
    print("prune_caches self-test OK")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--prefix", default="ccache-")
    ap.add_argument("--max-idle-days", type=int, default=7)
    ap.add_argument("--keep", type=int, default=2)
    ap.add_argument("--quota-gib", type=float, default=8.0)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()

    caches = gh_json("cache", "list", "--limit", "1000", "--json",
                     "id,key,ref,createdAt,lastAccessedAt,sizeInBytes")
    open_prs = {str(p["number"]) for p in gh_json("pr", "list", "--state", "open", "--limit", "500",
                                                  "--json", "number")}
    branches = {b["name"] for b in gh_json("api", "--paginate", "repos/{owner}/{repo}/branches")}
    now = dt.datetime.now(dt.timezone.utc)
    todo = plan(caches, now, open_prs, branches, a.prefix, a.max_idle_days, a.keep, a.quota_gib)

    mine = [c for c in caches if c["key"].startswith(a.prefix)]
    print(f"{len(caches)} caches ({len(mine)} managed, "
          f"{sum(c['sizeInBytes'] for c in mine) / GIB:.2f} GiB); deleting {len(todo)}")
    for c, why in todo:
        print(f"  {'would delete' if a.dry_run else 'delete'} [{why}] {c['key']} ({c['ref']}, "
              f"{c['sizeInBytes'] / 1024 ** 2:.0f} MiB)")
        if not a.dry_run:
            subprocess.run(["gh", "cache", "delete", str(c["id"])], check=True)


if __name__ == "__main__":
    sys.exit(main() or 0)
