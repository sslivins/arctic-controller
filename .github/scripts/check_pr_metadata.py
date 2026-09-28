#!/usr/bin/env python3
"""Reject pull requests whose title or description would make a useless changelog.

PRs are squash-merged with the PR title as the commit subject and the PR body as
the commit message, and create-release.yml builds the release notes from those
subjects by their conventional-commit prefix. A title like "Link each fault to
Arctic's article" therefore silently drops the change from the release notes.

This runs as the first job of ci.yml, before the ~45 minute hardware suite, so a
bad title fails in seconds rather than after the device tests.

Usage (CI): check_pr_metadata.py --repo OWNER/REPO --pr N
    Fetches the title/body live from the API (not the event payload), so a rerun
    after editing the PR sees the edited text.
Usage (local): check_pr_metadata.py --title "..." --body "..."
"""

import argparse
import json
import os
import re
import subprocess
import sys

# Must match the categories create-release.yml puts in the changelog. `chore` is
# accepted but deliberately left out of the release notes.
TYPES = ("feat", "fix", "refactor", "test", "docs", "ci", "chore")

TITLE_RE = re.compile(
    r"^(?P<type>[a-z]+)(?:\((?P<scope>[a-z0-9][a-z0-9._/-]*)\))?(?P<bang>!)?: (?P<subject>\S.*)$"
)

# Subjects that say nothing about what changed.
VAGUE_SUBJECTS = {
    "changes", "cleanup", "fix", "fixes", "fix bug", "fix bugs", "fix stuff",
    "improvements", "misc", "stuff", "tweaks", "update", "updates", "wip",
}

MIN_SUBJECT_CHARS = 12
MIN_BODY_CHARS = 40


def check_title(title: str) -> list[str]:
    title = title.strip()
    m = TITLE_RE.match(title)
    if not m:
        return [
            f'Title "{title}" is not a conventional commit. Start it with one of '
            f"{', '.join(t + ':' for t in TYPES)} (optionally scoped, e.g. "
            '"fix(i18n): spell out the fan label"). The release notes are built '
            "from this prefix."
        ]
    errors = []
    kind = m.group("type")
    subject = m.group("subject").strip()
    if kind not in TYPES:
        errors.append(
            f'"{kind}:" is not a recognised type; use one of {", ".join(TYPES)}. '
            "Other types never reach the release notes."
        )
    if re.search(r"\bwip\b|do not merge", title, re.IGNORECASE):
        errors.append("Title is marked work-in-progress; use a draft PR instead.")
    elif subject.lower().rstrip(".") in VAGUE_SUBJECTS or len(subject) < MIN_SUBJECT_CHARS \
            or len(subject.split()) < 2:
        errors.append(
            f'Subject "{subject}" is too vague to be a release note. Describe what '
            f"changed for the user in at least {MIN_SUBJECT_CHARS} characters."
        )
    return errors


def meaningful_body(body: str) -> str:
    """The body with template scaffolding removed: comments, headings, empty checkboxes."""
    body = re.sub(r"<!--.*?-->", "", body or "", flags=re.DOTALL)
    kept = []
    for line in body.splitlines():
        s = line.strip()
        if not s or re.fullmatch(r"#+.*", s) or re.fullmatch(r"[-*]\s*\[[ xX]?\]\s*", s):
            continue
        kept.append(s)
    return " ".join(kept)


def check_body(body: str) -> list[str]:
    text = meaningful_body(body)
    if len(text) < MIN_BODY_CHARS:
        return [
            "The PR description is empty or only the template. Explain what changed "
            f"and why (at least {MIN_BODY_CHARS} characters); it becomes the squash "
            "commit message on main."
        ]
    return []


def check(title: str, body: str) -> list[str]:
    return check_title(title) + check_body(body)


def fetch_pr(repo: str, number: str) -> tuple[str, str]:
    out = subprocess.run(
        ["gh", "api", f"repos/{repo}/pulls/{number}", "--jq", "{title: .title, body: .body}"],
        check=True, capture_output=True, text=True,
    ).stdout
    data = json.loads(out)
    return data["title"] or "", data["body"] or ""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo")
    ap.add_argument("--pr")
    ap.add_argument("--title")
    ap.add_argument("--body", default="")
    args = ap.parse_args()

    if args.title is not None:
        title, body = args.title, args.body
    elif args.repo and args.pr:
        title, body = fetch_pr(args.repo, args.pr)
    else:
        ap.error("pass --repo and --pr, or --title")

    errors = check(title, body)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write("### PR title and description\n\n")
            f.write(f"Title: `{title}`\n\n")
            if errors:
                f.write("".join(f"- :x: {e}\n" for e in errors))
                f.write("\nEdit the PR title/description; CI re-runs automatically.\n")
            else:
                f.write(":white_check_mark: Ready for the release notes.\n")

    for e in errors:
        print(f"::error title=PR title/description::{e}")
    if not errors:
        print(f"OK: {title}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
