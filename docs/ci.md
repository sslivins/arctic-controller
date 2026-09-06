# CI architecture

## The rule that shapes everything

GitHub offers two ways to not run a job, and branch protection treats them as
opposites:

| how the job is omitted | check run created? | branch protection sees |
| --- | --- | --- |
| an `on:`-level `paths:` filter excludes the change | **no** | the required context never reports, so the PR is **blocked forever** |
| a job-level `if:` evaluates false | **yes**, concluded `skipped` | **accepted as passing** |

Every design decision below follows from that asymmetry.

Both of its failure modes were live in this repository before September 2026:

* **Deadlock.** `build.yml` and `device-tests.yml` were both paths-filtered, and
  `web/**` appeared in neither filter. A PR touching only `web/**`, `docs/**`
  or a `*.md` file triggered no gating workflow, so the required `build` and
  `device-tests` contexts could never report. The PR showed every visible check
  green with nothing in flight, and could not be merged.
* **Vacuous pass.** The `device-tests` job depended on the firmware build, so a
  build *failure* turned it `skipped` -- which protection accepted. The
  hardware gate could report success having tested nothing.

## The shape of the solution

`ci.yml` is the single entry point for pull requests and the merge queue. It
has **no paths filter**, so it always reports, and it is the only workflow that
should ever be a required status check.

```
classify ──┬── call-build ────────────┐
           ├── call-host-tests ───────┼── ci-gate
           └── call-device-tests ─────┘   (if: always())
```

* **`classify`** lists the PR's changed files and decides whether the change is
  hardware-relevant. It uses an **exclusion** list, so anything not recognised
  as inert documentation counts as relevant: a new source directory is covered
  automatically rather than silently bypassing the device suite. It pages the
  file list, because a large PR would otherwise be truncated at 30 files and
  could be misclassified as documentation-only. An empty file list is an error.
* **`call-*`** invoke the heavy suites as reusable workflows.
* **`ci-gate`** runs with `if: always()` and *enumerates the acceptable outcome
  combinations*, failing on anything else. A skip has to be justified by the
  classification, not merely observed.

`device-tests.yml` has the same shape internally, ending in `device-gate`.

## Consequences worth knowing

**Nested check names.** A job called through `workflow_call` reports as
`call-device-tests / device-tests`, not `device-tests`. Contexts created
directly through the Checks API by `dorny/test-reporter` (`API Contract Test
Results` and friends) stay flat. None of these should be required: on a
documentation-only PR the device suite is skipped, so the flat test-reporter
contexts never report -- requiring them would reintroduce the deadlock.

**Caller permissions cap the callee.** A caller can only grant *downward*. If a
called workflow declares a permission the caller lacks, the run is rejected as
a `startup_failure` before any job exists -- which creates no check run, the
very condition this design removes. `call-device-tests` therefore grants
`contents: write`, which `device-firmware-build` needs solely to publish the
nightly OTA prerelease; the grant is scoped to that one caller job so the build
and host-test calls stay read-only.

**`device-tests.yml` keeps its direct `push` trigger.** `create-release.yml`
authorises a release by looking for a **push-event** run matching the exact
commit, and a `workflow_call` invocation produces no top-level run entry for
that query to find. Routing the push path through `ci.yml` would quietly break
releases.

**Fork pull requests fail rather than skip.** The device suite runs on a
self-hosted runner with physical access to the controller, so fork-authored
code must never execute there. A hardware-relevant fork PR is failed explicitly
by `ci-gate`, with instructions to re-push the commit to a branch in this
repository, rather than being allowed to pass unvalidated.

**The merge queue is covered.** `merge_group` is handled by `ci.yml`, so
enabling a merge queue later cannot deadlock on a context that only pull
requests produce.

## Changing the policy

`tests/test_ci_policy.py` extracts the gate scripts from the workflow YAML and
executes them, so the tests exercise the shipped policy rather than a
restatement of it. It also asserts the structural invariants: no paths filter
on `ci.yml`, no duplicate check-run names, each gate depending on every one of
its siblings, and every caller granting at least the permissions its callee
declares.

Run it with:

```sh
pytest tests/test_ci_policy.py
```
