#!/usr/bin/env python3
"""Recommend a minor version bump on feat: PRs that don't carry one.

create-release.yml tags whatever PROJECT_VER says on main and then bumps the
patch, so a new feature ships as a patch release unless someone raises the
minor version first. The release reuses the binary CI built for the merged
commit, so the bump has to be in the PR before CI runs; afterwards it costs a
whole extra CI cycle.

Advisory only: it never fails the build. In CI it keeps one PR comment up to
date (posting it when a bump is due, deleting it once the bump is made).

Usage (CI):    check_version_bump.py --repo OWNER/REPO --pr N
Usage (local): check_version_bump.py --title "feat: ..." --version 3.5.1 --released v3.5.0
"""

import argparse
import json
import os
import re
import subprocess
import sys

MARKER = "<!-- version-bump-hint -->"
FEATURE_RE = re.compile(r"^feat(?:\([^)]*\))?!?:|^[a-z]+(?:\([^)]*\))?!:")
VERSION_RE = re.compile(r'set\(PROJECT_VER\s+"(\d+)\.(\d+)\.(\d+)"\)')


def parse(version: str) -> tuple[int, int, int] | None:
    m = re.fullmatch(r"v?(\d+)\.(\d+)\.(\d+)", version.strip())
    return tuple(int(x) for x in m.groups()) if m else None


def recommendation(title: str, version: str, released: str | None) -> str | None:
    """The version to bump to, or None if no bump is due."""
    if not FEATURE_RE.match(title.strip()):
        return None
    cur, rel = parse(version), parse(released or "")
    if cur is None or rel is None:
        return None
    # A minor (or major) bump since the last release covers every feature
    # until the next one, so several feature PRs need only one bump.
    if cur[:2] > rel[:2]:
        return None
    return f"{rel[0]}.{rel[1] + 1}.0"


def gh(*args: str, input_text: str | None = None) -> str:
    return subprocess.run(["gh", *args], check=True, capture_output=True, text=True,
                          input=input_text).stdout


def project_ver(repo: str, sha: str) -> str:
    raw = gh("api", f"repos/{repo}/contents/CMakeLists.txt?ref={sha}",
             "-H", "Accept: application/vnd.github.raw")
    m = VERSION_RE.search(raw)
    return ".".join(m.groups()) if m else ""


def sync_comment(repo: str, pr: str, body: str | None) -> None:
    ids = gh("api", "--paginate", f"repos/{repo}/issues/{pr}/comments",
             "--jq", f'.[] | select(.body | contains("{MARKER}")) | .id').split()
    if body is None:
        for cid in ids:
            gh("api", "-X", "DELETE", f"repos/{repo}/issues/comments/{cid}")
        return
    payload = json.dumps({"body": f"{MARKER}\n{body}"})
    if ids:
        gh("api", "-X", "PATCH", f"repos/{repo}/issues/comments/{ids[0]}", "--input", "-",
           input_text=payload)
    else:
        gh("api", "-X", "POST", f"repos/{repo}/issues/{pr}/comments", "--input", "-",
           input_text=payload)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo")
    ap.add_argument("--pr")
    ap.add_argument("--title")
    ap.add_argument("--version")
    ap.add_argument("--released")
    args = ap.parse_args()

    if args.title is not None:
        title, version, released = args.title, args.version or "", args.released
    elif args.repo and args.pr:
        try:
            pr = json.loads(gh("api", f"repos/{args.repo}/pulls/{args.pr}",
                               "--jq", "{title: .title, sha: .head.sha}"))
            title, version = pr["title"] or "", project_ver(args.repo, pr["sha"])
            released = gh("api", f"repos/{args.repo}/releases/latest", "--jq", ".tag_name").strip()
        except subprocess.CalledProcessError as e:  # advisory: never fail CI over it
            print(f"::warning::version-bump check skipped: {e.stderr}")
            return 0
    else:
        ap.error("pass --repo and --pr, or --title/--version/--released")

    target = recommendation(title, version, released)
    message = None
    if target:
        message = (
            f"**Version bump recommended.** This is a feature PR, but `PROJECT_VER` is "
            f"`{version}` and the last release is `{released}`, so it would ship as a "
            f"patch release. Consider setting `PROJECT_VER` in `CMakeLists.txt` to "
            f"`{target}` in this PR; bumping after merge costs another full CI cycle."
        )
        print(f"::warning title=Version bump recommended::{message}")
    else:
        print(f"OK: no version bump needed ({title!r}, {version} vs {released})")

    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary and message:
        with open(summary, "a", encoding="utf-8") as f:
            f.write(f"### Version\n\n:warning: {message}\n")

    if args.repo and args.pr:
        try:
            sync_comment(args.repo, args.pr, message)
        except subprocess.CalledProcessError as e:  # advisory: never fail CI over it
            print(f"::warning::could not update the version-bump comment: {e.stderr}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
