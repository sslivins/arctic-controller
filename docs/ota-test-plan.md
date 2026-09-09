# OTA Test Plan

Status: proposed. Tracking issue: TBD (see "Work items" at the end).

## 1. Why this document exists

A controller in the field is reachable over exactly one path: its own network
stack. There is no serial console, no USB, and nobody on site with a cable.
That single fact drives everything below:

> **An OTA failure mode is only acceptable if the device recovers from it by
> itself.** "We can recover it over serial" is a valid statement about the CI
> rig and a meaningless one about a deployed device.

CI today recovers a failed OTA by reflashing over USB and then reports the job
as green. That is the correct behaviour for keeping the rig usable and the
wrong behaviour for telling us whether the firmware is shippable, because the
one capability the field depends on is the one the fallback silently replaces.

This plan enumerates the ways an OTA can go wrong, states what "graceful" means
for each, and says how we prove it.

## 2. What the device already does

Established by reading the build configuration and `main/ota_manager.cpp`;
these are facts about the current firmware, not aspirations.

| Mechanism | Setting | Consequence |
|---|---|---|
| Bootloader rollback | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` | A new image boots as `PENDING_VERIFY`. If it never marks itself valid, the bootloader reverts to the previous slot on the next boot. |
| App rollback API | `CONFIG_APP_ROLLBACK_ENABLE=y` | `esp_ota_mark_app_valid_cancel_rollback()` is available and is what commits an image. |
| Bootloader watchdog | `CONFIG_BOOTLOADER_WDT_ENABLE=y`, `CONFIG_BOOTLOADER_WDT_TIME_MS=9000` | A hang *inside the bootloader* is broken by a reset after 9s. |
| Anti-rollback | not set | No downgrade protection. An older image is accepted. |
| Secure boot | not set | Image signatures are not verified. Authenticity rests entirely on the API's authentication and URL allowlist. |
| Image validation | `esp_https_ota` / `esp_ota_*` | Magic byte, header and checksum are validated, so structurally invalid images are rejected before they can be booted. |

### Where the image is committed

`ota_mgr_mark_valid()` (`main/ota_manager.cpp:253`) is deliberately **not**
called during `app_main`. It is called from the UI loop
(`main/main.cpp:457`) once the startup animation has finished and
`create_ui()` has returned:

```
// NOW mark firmware as valid - display init succeeded, LVGL is
// running, UI rendered, heat-pump integration started, event log is up.
// If we got here, the firmware is functional.
```

This is a real health check and it is much better than marking valid at boot.
It is also, right now, the highest-risk line in the OTA system - see below.

## 3. The gap that matters most

**The health check that commits an image does not include network
reachability.**

The commit criteria are display init, LVGL running, UI rendered, heat-pump
integration started, event log up. All are local. An image that boots, renders
a perfect UI, and cannot join WiFi - or joins WiFi but fails to start the HTTP
server, or wedges its TCP stack - satisfies every one of them and permanently
cancels its own rollback.

What happens next makes it worse rather than better. `wifi_supervisor` escalates
a persistent outage to `system_safe_restart()` after 15 minutes
(`REBOOT_AFTER_MS`). Because the image is already marked valid, that reboot
returns to the *same* broken image. The device then loops: boot, render UI,
fail to reach the network, reboot 15 minutes later, forever. It is powered,
apparently alive, and unreachable, with rollback already surrendered.

This is precisely the field brick this plan exists to prevent, and no current
test would catch it, because CI only ever installs images whose network stack
works.

**Proposed change (to be validated by T-05):** extend the commit criteria to
require that the network is actually usable - association plus a successful
bind/listen on the API port, and ideally one completed request or a gateway
ping - subject to a generous timeout. If that timeout expires, do *not* mark
valid and reboot; the bootloader then rolls back to the last image known to
have been reachable. The timeout must comfortably exceed a slow DHCP lease and
a router still booting after a shared power cut, or we will roll back working
firmware over a transient outage. That trade-off is the substance of T-05 and
should be settled with measurements, not a guessed constant.

## 4. Current coverage, honestly stated

From the JUnit results of a green `device-tests` run and the workflow source:

| Area | State |
|---|---|
| OTA API surface: authentication, URL allowlist, rejection of malformed images, status schema | **Covered.** 43 tests pass. |
| An OTA that is actually installed and booted | **Only as a side effect** of the `Flash device via OTA` step, which is `continue-on-error: true`. |
| `TestOtaRoundTrip` (3 tests) | **Skipped**, reason "Requires serial connection and device reboot - not yet available on CI". The reason is stale: CI has serial capture and reboots the device every run. |
| `TestOtaRollback::test_rollback_on_crash_before_mark_valid` | **A stub.** Docstring plan, commented-out TODOs, ends in `pytest.skip`. |
| Rollback from an invalid or blank slot | **Not tested.** |
| Commit-without-network (section 3) | **Not tested.** |
| Power loss mid-write | **Not tested.** |

Both skip reasons classify as `deferred` under the T19 policy, so neither ever
fails the build. The result is 43 green OTA tests next to an untested rollback
path, which reads as coverage and is not.

One data point we did not have to write a test for: rollback-on-crash was
accidentally demonstrated during the #234 investigation. A diagnostic build was
OTA'd, panicked with an interrupt watchdog timeout before reaching
`create_ui()`, and the device came back on the previous image roughly 23
seconds later. The mechanism works. It is the *criteria* that need attention.

## 5. Failure-mode matrix

For each mode: how to induce it, what a graceful outcome is, and - the column
that decides whether we can ship - whether the device recovers **without
serial**.

| ID | Failure mode | Induce | Graceful outcome | Self-recovers in field? | Automatable on `ghr-mi` |
|---|---|---|---|---|---|
| F-01 | Malformed / truncated / garbage upload | POST random, empty, truncated, valid-header-bad-body | Rejected before write; running image untouched | Yes | Yes - already covered |
| F-02 | Download interrupted mid-transfer | Kill the connection partway through a pull-path update | Update abandoned, slot left unbootable-but-unused, running image untouched, state returns to idle | Yes | Yes |
| F-03 | Blank / invalid image in the inactive slot | Write zeros to the inactive slot over USB, set otadata to boot it | Bootloader refuses the image and boots the other slot | Yes | Yes (needs USB, so rig-only) |
| F-04 | Image boots, then crashes before commit | `poison_firmware.bin` - valid image that panics after boot, before `create_ui()` | Reboot, bootloader rolls back, device reachable on previous image | Yes | Yes |
| F-05 | Image boots, hangs without crashing | Poison variant that blocks a critical task and never reaches `create_ui()` | Watchdog fires, reboot, rollback | **Partly.** A *spinning* hang starves the idle task and the task WDT panics (60s). A *blocking* hang does not - see note below. | Yes |
| F-06 | Image boots and runs, network never comes up | Poison variant with the API server disabled or WiFi credentials nulled | Commit withheld, device reboots, rollback to reachable image | **No - this is the brick.** Section 3. | Yes |
| F-07 | Power loss mid-write | Cut power during the flash write | Next boot runs the untouched previous image; partial slot is rejected | Yes, by design (write is to the inactive slot) | Partially - needs switchable power |
| F-08 | Downgrade to an older image | OTA an older release | Accepted (no anti-rollback). Acceptable only because the API is authenticated. | n/a | Yes |
| F-09 | Unsigned / third-party image | OTA an image built elsewhere | Accepted (no secure boot). Authenticity rests on API auth plus URL allowlist. | n/a | Yes |

F-06 is the only entry in the matrix where the honest answer is "no". That is
the finding this plan is really about.

**Note on F-05.** The watchdogs are configured as
`CONFIG_ESP_TASK_WDT_EN=y`, `CONFIG_ESP_TASK_WDT_PANIC=y`,
`CONFIG_ESP_TASK_WDT_TIMEOUT_S=60`, with idle-task checks on both cores, plus
`CONFIG_ESP_INT_WDT=y` at 8000ms. That covers a hang that *spins*: the idle
task is starved, the task WDT panics, the device reboots and rolls back.

It does not cover a hang that *blocks*. If the boot path deadlocks on a mutex or
waits forever on a queue, the idle task still runs, no watchdog fires, and the
device sits there indefinitely: still in `PENDING_VERIFY`, never committed,
never rebooted, and therefore never rolled back. Rollback only happens on a
reboot, so an image that never crashes and never reboots is never reverted. If
the network happens to be up, `wifi_supervisor` will not save us either, because
its 15-minute backstop only fires while the network is *down*.

The fix is a boot-progress deadline: if the device has not reached its commit
criteria within a bounded time after an OTA, reboot. That naturally covers both
F-05 and F-06 and should be designed alongside T-05 rather than separately.

F-08 and F-09 are recorded as accepted risks rather than defects. Enabling
secure boot is a one-way, key-management-bearing decision and out of scope here;
it should be argued on its own merits, not slipped in as a test-plan side
effect.

## 6. Tests to build

Ordered by risk, not by effort.

- **T-01 - Un-skip `TestOtaRoundTrip`.** The stated blocker no longer exists.
  Upload, reboot, confirm the device returns on the new `build_sha`, confirm the
  image is marked valid. Cheap, and it turns the existing OTA flash from an
  incidental step into an asserted one.
- **T-02 - `poison_firmware.bin` fixture and F-04.** A deliberately built image
  that panics after boot and before `create_ui()`. Install it, assert the device
  returns on the *previous* `build_sha` within a bounded time. Fills in the
  existing stub. Build it as a CI artifact from a dedicated build flag so it is
  reproducible, and never publish it to a release.
- **T-03 - F-03, blank slot.** Zero the inactive slot over USB, point otadata at
  it, assert the bootloader falls back. Rig-only by nature; the API correctly
  refuses to help here, which is itself worth asserting.
- **T-04 - F-02, interrupted download.** Assert the state machine returns to
  idle and the running image is untouched, with no lock left held.
- **T-05 - F-06, the commit criteria.** First a host-level review and decision
  on what "healthy" must mean (section 3), then a poison variant that boots
  cleanly but cannot serve the API, asserting the device rolls back instead of
  committing. **This is the highest-value test in the plan and the one that
  changes firmware behaviour.**
- **T-06 - F-05, hang without crash.** Cover the blocking-hang case with the
  boot-progress deadline described under the matrix; a spinning hang is already
  covered by the task WDT and should be asserted rather than assumed.
- **T-07 - F-07, power loss.** Requires switchable power on the rig. Lowest
  priority: the A/B design makes this the best-understood mode, and the others
  buy more certainty per unit of effort.

Every device-level test must leave the rig on a known-good image. The recovery
procedure - write both OTA slots and erase otadata - is proven, and a known-good
binary is cached on the runner. Given that a diagnostic build bricked this same
controller during the #234 work, T-02, T-03, T-05 and T-06 all deliberately
brick the device and must each end with an explicit, asserted recovery step.

## 7. CI enforcement changes

The tests above are worth little while the workflow treats a failed OTA as a
pass.

1. **A failed OTA must fail the run.** `Flash device via OTA`
   (`.github/workflows/device-tests.yml`) is `continue-on-error: true`, and the
   USB fallback that follows makes the job green. Strict enforcement already
   exists but is gated on `github.event_name == 'schedule'`. Extend it to PR and
   push runs. Keep the fallback - the rig must stay usable - but record that it
   fired and fail the run when it does.
2. **Keep the rollback guard.** The `build_sha` identity check is what caught a
   silently rolled-back device during #234. It is the reason that failure was
   diagnosed instead of dismissed. Do not weaken it.
3. **Retire the stale skip reasons** as T-01 and T-02 land, so the executed-test
   floor reflects real coverage.
4. **Nightly keeps the pull path.** PR and push runs exercise the synchronous
   push path (`/api/ota/upload`); the asynchronous pull path (`/api/ota/update`
   from the `ci-nightly` prerelease) is nightly-only by design. That split is
   fine, but the pull path must not be the *only* place OTA installation is
   enforced.

## 8. Out of scope

Secure boot and anti-rollback (F-08, F-09) are accepted risks recorded above.
Fleet-wide staged rollout, update scheduling and delta updates are not
addressed. This plan covers a single device installing a single image and
surviving it.

## 9. Work items

| Item | Type | Priority |
|---|---|---|
| T-05 commit criteria: require network reachability before `mark_valid`, plus a boot-progress deadline | firmware | **Highest** |
| CI: fail PR/push runs on OTA failure | workflow | High |
| T-02 `poison_firmware.bin` + rollback-on-crash | test | High |
| T-01 un-skip `TestOtaRoundTrip` | test | High |
| T-03 blank-slot rollback | test | Medium |
| T-04 interrupted download | test | Medium |
| T-06 hang-without-crash coverage | investigation | Medium |
| T-07 power-loss | test (needs hardware) | Low |
