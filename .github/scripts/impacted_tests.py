#!/usr/bin/env python3
"""Pick the bench tests a pull request most likely affects.

device-tests.yml runs these before the full suites, so a PR that breaks them
fails its "PR-Impacted Test Results" check within minutes rather than at the
end of an hour-long run. The full suites still run and still decide the job;
this only moves the likeliest failures to the front.

A test file is picked when the PR adds or modifies it, or when it mentions
strings from the PR's source changes:

  * string literals on and around the changed lines (a changed timezone list
    sits next to "US Pacific (PST/PDT)", which the timezone test checks for);
  * widget tags, JSON keys, API paths and element ids from the whole of a
    small source file, because tests often address a different part of the
    screen (the roller's tag) from the part that changed (its entries). Large
    files (the API server, the dashboard) contribute only their changed hunks.

A string that appears in too many test files says nothing about which ones
matter and is ignored. Each changed source file takes turns adding its
best-matching tests, so one large diff can't crowd out a small one.

Usage: impacted_tests.py --base <rev> [--head <rev>]   (one test file per line)
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SUITES = ("tests/device/", "tests/api/", "tests/web/")
SOURCE_PREFIXES = ("main/", "components/")
SOURCE_SUFFIXES = (".c", ".cpp", ".h", ".hpp", ".html", ".js", ".css", ".yaml")
MAX_FILES = 8
MAX_FANOUT = 6
WHOLE_FILE_MAX_LINES = 1500
HUNK_CONTEXT = 20
HUNK_WEIGHT = 2.0

_TAG = re.compile(r"""["']([a-z][a-z0-9]*(?:[_-][a-z0-9]+)+)["']""")
_API = re.compile(r"(/api/[A-Za-z0-9_/-]*[A-Za-z0-9_])")
_HTML_ID = re.compile(r"""\b(?:id|for|data-tag)=["']([A-Za-z][\w-]{3,})["']""")
_LITERAL = re.compile(r'"([^"\\\n]{4,60})"')


def tokens(text: str) -> set[str]:
    """Identifiers in source text that tests address: tags, keys, paths, ids."""
    found = set(_TAG.findall(text)) | set(_HTML_ID.findall(text))
    found |= {p.rstrip("/") for p in _API.findall(text) if p.count("/") >= 3}
    return found


def literals(text: str) -> set[str]:
    """Quoted strings with some letters in them (labels, POSIX zones, keys)."""
    return {s for s in _LITERAL.findall(text) if re.search(r"[A-Za-z]{2}", s)}


def weigh(whole_text: str | None, hunk_text: str) -> dict[str, float]:
    """Strings a change is likely to affect, weighted by how close they are to it."""
    weights = {t: 1.0 for t in tokens(whole_text if whole_text is not None else hunk_text)}
    for t in tokens(hunk_text) | literals(hunk_text):
        weights[t] = HUNK_WEIGHT
    return weights


def mentions(test_text: str, token: str) -> bool:
    if token not in test_text:
        return False
    return re.search(r"(?<![\w-])" + re.escape(token) + r"(?![\w-])", test_text) is not None


def _ranked_for(weights: dict[str, float], test_texts: dict[str, str]) -> list[str]:
    """Tests ordered by how specifically they hit one source file's strings."""
    score: dict[str, float] = {}
    for tok, w in weights.items():
        files = [f for f, text in test_texts.items() if mentions(text, tok)]
        if 0 < len(files) <= MAX_FANOUT:
            for f in files:
                score[f] = score.get(f, 0.0) + w / len(files)
    return sorted(score, key=lambda f: (-score[f], f))


def rank(changed_tests: list[str], weights_by_source: list[dict[str, float]],
         test_texts: dict[str, str], limit: int = MAX_FILES) -> list[str]:
    """Changed tests first, then each changed source file's best tests in turn."""
    picked = [t for t in changed_tests if t in test_texts]
    queues = [_ranked_for(w, test_texts) for w in weights_by_source]
    while len(picked) < limit and any(queues):
        for q in queues:
            while q and q[0] in picked:
                q.pop(0)
            if q and len(picked) < limit:
                picked.append(q.pop(0))
    return picked


def _git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True,
                          text=True, encoding="utf-8", errors="replace").stdout


def _is_suite_test(path: str) -> bool:
    return path.startswith(SUITES) and Path(path).name.startswith("test_") and path.endswith(".py")


def _is_source(path: str) -> bool:
    return path.startswith(SOURCE_PREFIXES) and path.endswith(SOURCE_SUFFIXES) \
        and "/tests/" not in path


def _source_weights(base: str, head: str, path: str) -> dict[str, float]:
    diff = _git("diff", f"-U{HUNK_CONTEXT}", base, head, "--", path)
    hunk = "\n".join(l[1:] for l in diff.splitlines()
                     if l[:1] in " +-" and not l.startswith(("+++", "---")))
    whole = _git("show", f"{head}:{path}")
    return weigh(whole if whole.count("\n") <= WHOLE_FILE_MAX_LINES else None, hunk)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--base", required=True, help="revision the PR is compared against")
    ap.add_argument("--head", default="HEAD", help="revision under test (default: HEAD)")
    ap.add_argument("--limit", type=int, default=MAX_FILES)
    args = ap.parse_args(argv)

    changed = _git("diff", "--name-only", "--diff-filter=AMR", args.base, args.head).split()
    test_texts = {
        p.relative_to(ROOT).as_posix(): p.read_text(encoding="utf-8", errors="replace")
        for suite in SUITES for p in sorted((ROOT / suite).glob("test_*.py"))
    }
    changed_tests = [c for c in changed if _is_suite_test(c)]
    weights = [_source_weights(args.base, args.head, c) for c in changed if _is_source(c)]

    for f in rank(changed_tests, weights, test_texts, args.limit):
        print(f)
    return 0


if __name__ == "__main__":
    sys.exit(main())
