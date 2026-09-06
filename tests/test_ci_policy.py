"""Policy tests for the CI gating workflows.

These exist because the CI gates encode *safety* policy, not convenience:

* An ``on:``-level ``paths:`` filter creates NO check run, so a required
  status check never reports and the PR is blocked forever.
* A job-level ``if:`` skip DOES create a check run, with conclusion
  ``skipped``, which branch protection ACCEPTS as passing.

That asymmetry previously allowed both a permanent deadlock (a docs-only or
web-only PR triggered no gating workflow at all) and a vacuous pass (a failed
firmware build turned ``device-tests`` into ``skipped``, which counted as
green -- the hardware gate passing having tested nothing).

The gate shell scripts are extracted from the workflow YAML and executed, so
these tests exercise the real shipped policy rather than a restatement of it.
"""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest
import yaml

WORKFLOWS = Path(__file__).parents[1] / ".github" / "workflows"

pytestmark = [
    pytest.mark.hostside,
    pytest.mark.skipif(
        shutil.which("bash") is None and shutil.which("wsl") is None,
        reason="needs bash to execute the gate policy",
    ),
]


def _bash(script_path: Path) -> list[str]:
    """Build the argv that runs ``script_path`` under a real POSIX bash.

    On Linux (CI) this is simply ``bash <path>``. On Windows the ``bash.exe``
    on PATH is the WSL launcher, which neither inherits the Windows
    environment nor understands Windows paths, so the interpreter is invoked
    through ``wsl`` with a translated path. Scripts are always executed from a
    file: passing them via ``-c`` would expose them to a second round of
    command-line parsing by that launcher.
    """
    if os.name != "nt":
        return ["bash", str(script_path)]
    drive, rest = os.path.splitdrive(script_path)
    posix = "/mnt/" + drive[0].lower() + rest.replace("\\", "/")
    return ["wsl", "-e", "bash", posix]


def _bash_run(body: str, env: dict[str, str], stdin: str = "") -> subprocess.CompletedProcess:
    prelude = "".join(f"export {k}={shlex.quote(v)}\n" for k, v in env.items())
    fd, name = tempfile.mkstemp(suffix=".sh", text=False)
    path = Path(name)
    try:
        with os.fdopen(fd, "wb") as fh:
            # Force LF: a CR would become part of the final token on each line.
            fh.write((prelude + body + "\n").replace("\r\n", "\n").encode())
        return subprocess.run(
            _bash(path), input=stdin, capture_output=True, text=True
        )
    finally:
        path.unlink(missing_ok=True)


def _workflow(name: str) -> dict:
    with open(WORKFLOWS / name, encoding="utf-8") as fh:
        return yaml.safe_load(fh)


def _triggers(doc: dict) -> dict:
    # PyYAML parses the bare `on:` key as the boolean True.
    return doc.get(True, doc.get("on"))


def _gate_script(workflow: str, job: str) -> str:
    return _workflow(workflow)["jobs"][job]["steps"][0]["run"]


def _run_gate(script: str, env: dict[str, str]) -> int:
    return _bash_run(script, env).returncode


# --------------------------------------------------------------------------
# Structural invariants
# --------------------------------------------------------------------------


def test_ci_orchestrator_has_no_paths_filter():
    """ci.yml must always report, or a required ci-gate deadlocks the PR."""
    triggers = _triggers(_workflow("ci.yml"))
    for event in ("pull_request", "merge_group"):
        assert event in triggers, f"ci.yml must trigger on {event}"
    pr = triggers["pull_request"] or {}
    assert "paths" not in pr and "paths-ignore" not in pr, (
        "ci.yml must NOT have a paths filter: an on:-level skip creates no "
        "check run, so the required ci-gate context would never report and "
        "the pull request would be blocked permanently."
    )


@pytest.mark.parametrize("name", ["build.yml", "device-tests.yml", "host-tests.yml"])
def test_called_workflows_do_not_also_trigger_on_pull_request(name):
    """Double-triggering would run the ~40 min hardware suite twice.

    device-tests.yml has a global concurrency group and there is a single
    physical controller, so a duplicate run serialises rather than parallelises.
    """
    triggers = _triggers(_workflow(name))
    assert "pull_request" not in triggers, (
        f"{name} is invoked by ci.yml via workflow_call; a pull_request "
        "trigger would duplicate it."
    )
    assert "workflow_call" in triggers, f"{name} must be callable from ci.yml"


def test_device_tests_keeps_direct_push_trigger():
    """create-release.yml resolves attestations via a top-level push run.

    A workflow_call invocation produces no top-level run entry, so routing the
    push path through ci.yml would silently break releases.
    """
    assert "push" in _triggers(_workflow("device-tests.yml"))


def test_no_duplicate_check_run_names_across_workflows():
    """Two check runs sharing a name make a required context ambiguous."""
    seen: dict[str, str] = {}
    duplicates = []
    for wf in ("build.yml", "device-tests.yml", "host-tests.yml", "ci.yml"):
        for job in _workflow(wf)["jobs"]:
            if job in seen:
                duplicates.append(f"{job!r} in both {seen[job]} and {wf}")
            seen[job] = wf
    assert not duplicates, "duplicate job/check-run names: " + "; ".join(duplicates)


def test_caller_grants_at_least_the_permissions_the_callee_declares():
    """A caller can only grant downward.

    If a called workflow declares a permission the caller does not hold, the
    entire run is rejected with a `startup_failure` before any job is created
    -- and that produces no check run at all, which is precisely the
    never-reports condition this migration exists to eliminate.
    """
    ci = _workflow("ci.yml")
    rank = {"none": 0, "read": 1, "write": 2}
    calls = {
        job["uses"].rsplit("/", 1)[-1]: (name, job)
        for name, job in ci["jobs"].items()
        if "uses" in job
    }
    assert calls, "ci.yml must call the heavy suites as reusable workflows"

    problems = []
    for wf_name, (caller_job, job) in calls.items():
        granted = {**(ci.get("permissions") or {}), **(job.get("permissions") or {})}
        callee = _workflow(wf_name)
        # The callee's requirement is the highest level any of its jobs asks for.
        required: dict[str, str] = dict(callee.get("permissions") or {})
        for inner in (callee.get("jobs") or {}).values():
            for scope, level in (inner.get("permissions") or {}).items():
                if rank.get(level, 0) > rank.get(required.get(scope, "none"), 0):
                    required[scope] = level
        for scope, level in required.items():
            have = granted.get(scope, "none")
            if rank.get(have, 0) < rank.get(level, 0):
                problems.append(
                    f"{caller_job} -> {wf_name}: needs {scope}:{level}, caller grants {scope}:{have}"
                )
    assert not problems, "insufficient permissions: " + "; ".join(problems)


def test_ci_gate_depends_on_every_other_job():
    """A gate that does not observe a job cannot enforce anything about it."""
    jobs = _workflow("ci.yml")["jobs"]
    gate = jobs["ci-gate"]
    assert gate["if"] == "always()", "ci-gate must run even when a dependency fails"
    expected = {j for j in jobs if j != "ci-gate"}
    assert set(gate["needs"]) == expected


def test_device_gate_depends_on_every_other_job():
    jobs = _workflow("device-tests.yml")["jobs"]
    gate = jobs["device-gate"]
    assert gate["if"] == "always()"
    expected = {j for j in jobs if j != "device-gate"}
    assert set(gate["needs"]) == expected


# --------------------------------------------------------------------------
# ci-gate policy
# --------------------------------------------------------------------------

CI_OK = {
    "CLASSIFY": "success",
    "HW": "true",
    "FORK": "false",
    "BUILD": "success",
    "HOST": "success",
    "DEVICE": "success",
}


@pytest.mark.parametrize(
    "override,reason",
    [
        ({}, "hardware-relevant change with everything green"),
        (
            {"HW": "false", "DEVICE": "skipped"},
            "documentation-only change correctly skips the device suite",
        ),
    ],
)
def test_ci_gate_accepts_valid_outcomes(override, reason):
    assert _run_gate(_gate_script("ci.yml", "ci-gate"), {**CI_OK, **override}) == 0, reason


@pytest.mark.parametrize(
    "override,reason",
    [
        ({"DEVICE": "skipped"}, "VACUOUS PASS: hardware relevant but suite skipped"),
        ({"DEVICE": "failure"}, "device suite failed"),
        ({"DEVICE": "cancelled"}, "device suite cancelled"),
        ({"BUILD": "failure"}, "build failed"),
        ({"BUILD": "skipped"}, "build skipped"),
        ({"HOST": "failure"}, "host tests failed"),
        ({"HOST": "skipped"}, "host tests skipped"),
        ({"CLASSIFY": "failure"}, "classifier failed"),
        ({"CLASSIFY": "skipped"}, "classifier skipped"),
        ({"CLASSIFY": "cancelled"}, "classifier cancelled"),
        ({"HW": ""}, "empty hw_relevant must not be treated as false"),
        ({"HW": "yes"}, "non-literal hw_relevant"),
        ({"FORK": ""}, "empty is_fork"),
        (
            {"FORK": "true", "DEVICE": "skipped"},
            "fork PR touching hardware cannot be validated and must not pass",
        ),
        (
            {"HW": "false", "DEVICE": "success"},
            "device ran although classified irrelevant: policy disagreement",
        ),
    ],
)
def test_ci_gate_rejects_invalid_outcomes(override, reason):
    assert _run_gate(_gate_script("ci.yml", "ci-gate"), {**CI_OK, **override}) != 0, reason


# --------------------------------------------------------------------------
# device-gate policy (also guards the direct push-to-main release path)
# --------------------------------------------------------------------------

DEV_OK = {
    "PREFLIGHT": "success",
    "BUILD": "success",
    "DEVICE": "success",
    "REUSE": "skipped",
    "DEDUP": "false",
}

DEV_REUSE = {
    "PREFLIGHT": "success",
    "BUILD": "skipped",
    "DEVICE": "skipped",
    "REUSE": "success",
    "DEDUP": "true",
}


@pytest.mark.parametrize(
    "env,reason",
    [
        (DEV_OK, "physical suite ran and passed"),
        (DEV_REUSE, "validated tree reused, attestation re-emitted"),
    ],
)
def test_device_gate_accepts_valid_outcomes(env, reason):
    assert _run_gate(_gate_script("device-tests.yml", "device-gate"), env) == 0, reason


@pytest.mark.parametrize(
    "override,reason",
    [
        (
            {"BUILD": "failure", "DEVICE": "skipped"},
            "VACUOUS PASS: build failed so the suite never ran",
        ),
        (
            {"BUILD": "skipped", "DEVICE": "skipped"},
            "VACUOUS PASS: nothing ran but dedup was not claimed",
        ),
        ({"DEVICE": "failure"}, "device suite failed"),
        ({"DEVICE": "cancelled"}, "device suite cancelled"),
        ({"PREFLIGHT": "failure"}, "preflight failed"),
        ({"DEDUP": ""}, "empty dedup must not be treated as false"),
        ({"DEDUP": "maybe"}, "non-literal dedup"),
        ({"REUSE": "success"}, "reuse ran although dedup=false"),
    ],
)
def test_device_gate_rejects_invalid_outcomes(override, reason):
    env = {**DEV_OK, **override}
    assert _run_gate(_gate_script("device-tests.yml", "device-gate"), env) != 0, reason


@pytest.mark.parametrize(
    "override,reason",
    [
        ({"REUSE": "failure"}, "dedup claimed but the attestation was not re-emitted"),
        ({"BUILD": "success", "DEVICE": "success"}, "dedup claimed but the suite ran"),
    ],
)
def test_device_gate_rejects_invalid_reuse_outcomes(override, reason):
    env = {**DEV_REUSE, **override}
    assert _run_gate(_gate_script("device-tests.yml", "device-gate"), env) != 0, reason


# --------------------------------------------------------------------------
# Classifier relevance policy
# --------------------------------------------------------------------------


def _non_hw_regex() -> str:
    import re

    script = _workflow("ci.yml")["jobs"]["classify"]["steps"][0]["run"]
    match = re.search(r"NON_HW='([^']*)'", script)
    assert match, "could not find NON_HW in ci.yml"
    return match.group(1)


def _is_relevant(files: list[str]) -> bool:
    # The list is emitted by printf rather than piped on stdin: stdin is not
    # reliably forwarded to the interpreter on every platform, and a silently
    # empty list would read as "nothing relevant" -- the exact false-negative
    # this test exists to catch.
    listing = " ".join(shlex.quote(f) for f in files) or "''"
    script = (
        "REL=false\n"
        f"for f in {listing}; do\n"
        '  [ -z "$f" ] && continue\n'
        '  if echo "$f" | grep -Eq "$NON_HW"; then :; else REL=true; fi\n'
        "done\n"
        'echo "$REL"\n'
    )
    proc = _bash_run(script, {"NON_HW": _non_hw_regex()})
    assert proc.returncode == 0, proc.stderr
    return proc.stdout.strip() == "true"


@pytest.mark.parametrize(
    "files",
    [
        ["README.md"],
        ["docs/architecture.md"],
        ["LICENSE"],
        [".gitignore"],
        [".github/ISSUE_TEMPLATE/bug.yml"],
        ["tests/api/README.md"],
    ],
)
def test_inert_documentation_does_not_require_hardware(files):
    assert not _is_relevant(files)


@pytest.mark.parametrize(
    "files,reason",
    [
        (["main/heatpump_controller.cpp"], "firmware source"),
        (["tests/api/test_api_schema.py"], "tests-only PR, cf. PR #214"),
        (["web/index.html"], "web dashboard: absent from BOTH old paths filters"),
        ([".github/workflows/ci.yml"], "a workflow change must revalidate"),
        (["CMakeLists.txt"], "build configuration"),
        (["sdkconfig.defaults"], "sdkconfig"),
        (["partitions.csv"], "partition table"),
        (["components/arctic-macon"], "submodule bump"),
        (["docs/a.md", "main/b.cpp"], "one relevant file in a mixed PR wins"),
        (["scripts/flash.sh"], "unknown directory must default to relevant"),
        (["Makefile"], "unrecognised root file must default to relevant"),
    ],
)
def test_source_changes_require_hardware(files, reason):
    assert _is_relevant(files), reason
