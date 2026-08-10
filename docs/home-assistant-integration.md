# Home Assistant Local Integration Architecture

Status: Accepted for phased implementation

## Decision

The Arctic Controller will support a local-only Home Assistant custom
integration using a hybrid transport:

1. REST provides the initial state snapshot, capabilities, and commands.
2. A device-hosted WebSocket provides immediate state updates.
3. Periodic REST reconciliation repairs missed updates and provides fallback
   operation while the WebSocket is unavailable.

MQTT, cloud relays, Home Assistant webhooks, Matter, and the ESPHome native
protocol are not required.

## Goals

- Immediate local state updates without a broker or cloud dependency.
- Correctly distinguish selected working mode from actual heat-pump operation.
- Remain useful during WebSocket interruption through bounded fallback polling.
- Keep the device UI, Web UI, heat-pump communication, and OTA responsive when
  Home Assistant is absent, disconnected, slow, or malicious.
- Require explicit, revocable authentication even on a trusted home network.
- Provide stable, versioned contracts that can evolve without breaking older
  integrations.
- Prevent stale reconciliation responses from overwriting newer pushed state.

## Non-goals

- Cloud control or telemetry.
- Generic register access from Home Assistant.
- Reading or writing advanced/technician parameters from Home Assistant.
- Replacing the device UI or Web UI.
- Making Home Assistant part of the heat-pump control loop.
- Browser use of the integration WebSocket.

## Home Assistant Control Allowlist

Home Assistant may eventually control only:

- Unit power.
- Selected working mode.
- Cooling setpoint.
- Heating setpoint.
- Hot-water setpoint.

All controls remain disabled until the read-only integration has completed its
security gate. Commands are non-optimistic: a successful REST response means
the command was accepted, not that the reported heat-pump state has changed.

The following are permanently excluded:

- Generic register writes.
- Advanced or technician parameter writes.
- Raw Macon/Tuya access.
- OTA, Wi-Fi, authentication, TLS, factory reset, and device preferences.

## State Semantics

The integration must preserve these independent concepts:

- `mode`: selected working-mode policy.
- `operation`: actual live operation decoded from heat-pump telemetry.

Home Assistant maps selected mode to the climate entity's requested HVAC mode
and actual operation to `hvac_action`. It must never infer operation from mode.

The initial exposed snapshot includes:

- Connection and availability state.
- Unit power.
- Selected mode and actual operation.
- Defrost and component state.
- Temperatures.
- Setpoints and setpoint limits.
- Compressor frequency, power, thermal output, and COP when valid.
- Current error state.
- Firmware, protocol, and capability metadata.

Diagnostic entities that are noisy or primarily useful for troubleshooting
should be disabled by default.

## Versioned REST Contract

### `GET /api/v1/capabilities`

Returns:

- Stable device ID.
- Model and firmware version.
- API protocol version.
- Supported transports.
- Supported state fields.
- Read and control capabilities.
- Setpoint limits.

The stable device ID is derived from a hardware identity and does not depend on
hostname, IP address, or Home Assistant configuration.

### `GET /api/v1/state`

Returns one coherent snapshot built by the same serializer used by WebSocket
snapshot messages.

Every snapshot includes:

- `protocol_version`
- `device_id`
- `boot_id`
- `revision`
- `captured_at_ms`
- `state`

`boot_id` changes on every controller boot. `revision` increases whenever any
exposed state field changes. The revision is assigned while the state snapshot
is coherent, but serialization and network transmission occur without holding
the heat-pump state mutex.

The Home Assistant client discards a REST or WebSocket snapshot when:

- Its device ID does not match the configured device.
- Its boot ID matches the current boot but its revision is not newer.
- Its protocol version is unsupported.

### Commands

The allowlisted controls use versioned endpoints rather than generic register
writes. Each request includes a client-generated command ID and each response
reports accepted/rejected status.

The device includes the most recent applicable command ID in a subsequent
snapshot when practical. If confirmation is not observed within a bounded
interval, Home Assistant performs a reconciliation request and reports failure
instead of fabricating success.

## WebSocket Contract

### Endpoint

`GET /api/v1/events`

The endpoint is intended for native clients such as Home Assistant. It uses an
authenticated HTTP upgrade and does not accept credentials in the URL, query
string, WebSocket subprotocol, or application frames.

### Connection sequence

1. Authenticate the HTTP upgrade.
2. Send `hello` containing protocol version, device ID, boot ID, and current
   revision.
3. Send a complete state snapshot.
4. Send complete snapshots when exposed state changes.
5. Exchange heartbeat ping/pong frames.

Complete snapshots are used initially instead of deltas. The state payload is
small enough for local transport, and complete snapshots avoid divergent
client state and complicated schema migration.

### Ordering and recovery

- Revisions are monotonic within one boot.
- A new boot ID invalidates the previous revision sequence.
- A missing or out-of-order revision causes REST resynchronization.
- A protocol-version change causes capability negotiation before accepting new
  state.
- A periodic full snapshot bounds state age even when exposed values do not
  change.

### Backpressure

- WebSocket sends never occur while holding the heat-pump state mutex.
- Each client has a small bounded queue.
- State is latest-value-wins; stale queued snapshots may be replaced.
- A slow client cannot block the HTTP worker, UI, Web UI, heat-pump tasks, or
  another client.
- Sends use a hard local timeout. A stalled or failed socket is disconnected.
- Queue overflow triggers resynchronization or disconnection rather than
  unbounded allocation.
- Concurrent clients are capped.

The feasibility spike selected a separate server, task, port, and socket pool.
Production integration traffic will use WSS on port 8443. The test-only spike
uses plain WebSocket on port 81 when `CONFIG_TEST_ENDPOINTS` is enabled.

The integration server uses:

- A four-socket pool independent of the Web UI and REST servers.
- A 100 ms per-socket send deadline.
- A small concurrent-client cap.
- Latest-value-wins queues and disconnection after a stalled send.

The shared-server prototype failed because one non-reading client produced a
2.26-second REST response and one REST timeout. With the isolated server and
bounded socket send, the 15-second stress gate had no REST or healthy-client
failures. REST p95 was 229 ms with a 370 ms maximum; healthy WebSocket p95 was
152 ms with a 905 ms maximum. A follow-up run that also fetched the complete
Web UI throughout the stall had zero failures, 207 ms p95, and a 293 ms
maximum.

Ten additional connect/stall/disconnect cycles had no failures. Across those
cycles, worst REST p95 was 305 ms, worst REST latency was 1.36 seconds, worst
healthy-WebSocket p95 was 556 ms, worst healthy-WebSocket latency was 943 ms,
and free-heap drift was 3,144 bytes. Minimum free heap remained 27,704,772
bytes.

Enabling ESP-IDF WebSocket support enlarged every `httpd_uri_t` and exposed
that route registration was exhausting the 4 KB system-event task stack. The
spike uses an 8 KB event-task stack. Production work must move server creation
and route registration out of the Wi-Fi event callback into a dedicated
startup task rather than relying on further event-stack growth.

## Home Assistant Update Model

The integration is classified as `local_push` and its entities do not use Home
Assistant entity polling.

The client performs:

- One REST snapshot during setup.
- Immediate coordinator updates from accepted WebSocket snapshots.
- REST reconciliation every 60 seconds while the stream is healthy.
- Temporary 10-15 second REST polling while the stream is unavailable.
- Exponential WebSocket reconnect backoff with jitter.
- Immediate reconciliation after sequence gaps, reboot, or command timeout.

REST and WebSocket updates pass through one ordering gate so a delayed REST
response cannot regress state after a newer push update.

## Discovery and Configuration

The controller advertises zeroconf metadata containing:

- Device type.
- Stable device ID.
- Protocol version.
- HTTPS availability.
- WebSocket push capability.

Discovery never advertises credentials, tokens, certificate material, state,
or identifying network information beyond what zeroconf requires.

The Home Assistant config flow supports:

- Zeroconf discovery and manual host entry.
- Duplicate-device detection by stable device ID.
- Secure pairing.
- Host and IP address changes.
- Token rotation and reauthentication.
- Certificate fingerprint changes that require explicit user confirmation.
- Clean unload and reload without orphaned tasks or sockets.

## Security Model

### Dedicated integration credentials

The Home Assistant API uses a dedicated random 256-bit token:

- Generated only after a six-digit one-time code from the physical controller
  is claimed during a five-minute pairing window.
- Returned once during pairing.
- Stored as a cryptographic hash on the controller.
- Compared in constant time.
- Stored in Home Assistant config-entry storage.
- Redacted from logs, exceptions, traces, and diagnostics.
- Independently revocable and rotatable.

Integration endpoints use a strict authentication helper that always requires
the dedicated token. Existing web/API authentication toggles cannot bypass it.
The pairing endpoint is the sole exception: it is TLS-only, accepts only the
controller-displayed one-time code, closes after one successful claim, and
closes after five invalid attempts. Opening a new window does not revoke the
current token; a successful claim atomically rotates it.
If the client disconnects after a successful claim but before receiving the
one-time token, the user reopens pairing on the controller and claims a new
code.

### Transport identity

Pairing and authenticated integration traffic require TLS. The supported local
identity model is certificate/public-key fingerprint pinning:

- The controller automatically generates a dedicated P-256 self-signed
  integration certificate and private key on first boot.
- The identity is stored in NVS and served only by the dedicated integration
  HTTPS/WSS server on port 8443.
- It is independent of the optional manually uploaded Web UI certificate on
  port 443; changing or deleting that certificate does not disrupt Home
  Assistant.
- Home Assistant records the fingerprint during the physically authorized
  pairing flow.
- Subsequent connections reject a changed fingerprint.
- Fingerprint pinning does not depend on the controller having a correct clock.
- Plain HTTP does not silently downgrade from a configured secure connection.

There is no production insecure mode and no certificate renewal workflow.
The integration identity changes only after factory reset or an explicit future
identity-reset action. Test firmware may expose HTTP-only provisioning helpers,
but production integration state and events remain HTTPS/WSS-only.

### Threats and mitigations

| Threat | Mitigation |
|---|---|
| LAN eavesdropping or token theft | TLS-only pairing and traffic; no query-string credentials |
| LAN man-in-the-middle | Physically authorized fingerprint pinning |
| Stolen Home Assistant backup | Revocable dedicated token; documented rotation |
| Replay | TLS plus command IDs and current authenticated session |
| Weak global API settings | Strict integration auth independent of existing toggles |
| Malicious or slow client | Client cap, bounded queues, send timeout, disconnect policy |
| Socket exhaustion | Hardware feasibility gate and reserved UI/API capacity |
| Oversized or malformed frames | Frame-size limits and strict schema validation |
| Stale state overwrite | Shared boot ID and revision ordering gate |
| Firmware schema change | Version negotiation and capability refresh |
| Secret disclosure | Redacted diagnostics/logging and one-time token delivery |
| Unauthorized heat-pump changes | Explicit command allowlist; no AP or register writes |

Before control entities are enabled, existing legacy control endpoints must
either use equivalent strict authentication or be fenced from the integration
security boundary.

## Implementation Phases

### Phase 1: Protocol and threat model

- Finalize this contract and message schemas.
- Define stable device identity and revision ownership.
- Document pairing, pinning, token lifecycle, and legacy endpoint policy.
- Define measurable acceptance and abuse tests.

### Phase 2: Hardware feasibility gate

- Prototype persistent WebSocket connections on the physical controller.
- Exercise two persistent clients plus Web UI and REST traffic.
- Simulate a stalled/zero-window client.
- Measure socket usage, request latency, free heap, minimum heap, and
  fragmentation during sustained full-snapshot traffic.
- Select shared-server or separate-server architecture.

Status: complete. The separate-server architecture passed the gate. Production
TLS and authentication are implemented in Phase 3 before the endpoint is
exposed outside test builds.

### Phase 3: Security and state foundation

- Implement strict integration authentication.
- Implement dedicated token pairing, hashing, revocation, and rotation.
- Implement TLS fingerprint identity behavior.
- Add stable device identity, state revision, shared serializer, capabilities,
  state endpoint, and zeroconf metadata.

Status: complete. The dedicated 256-bit token is stored only as a SHA-256
hash, strict Bearer validation is independent of legacy authentication
toggles, token rotation invalidates the previous credential immediately, and
the versioned capabilities/state endpoints now expose stable device identity,
per-boot identity, and monotonic revisions. API startup also runs on a
dedicated task rather than the system event stack. The automatic integration
TLS identity, dedicated port 8443 server, certificate fingerprint, and
zeroconf metadata are also implemented. The physical settings flow opens a
five-minute pairing window, displays a six-digit one-time code and certificate
fingerprint, limits invalid claims, supports cancellation and revocation, and
rotates the credential only after a successful TLS claim. Home Assistant-side
pin confirmation is implemented later with the async client and config flow.

### Phase 4: Device push transport

- Implement WebSocket hello, snapshot, heartbeat, ordering, coalescing,
  backpressure, disconnect, and resynchronization.
- Verify telemetry-only changes produce push updates.
- Run physical reliability and resource tests.

### Phase 5: Async Python client

- Implement secure REST/WSS, pinning, ordering, reconnect, reconciliation,
  fallback polling, cancellation, and typed models.
- Test against a deterministic fake server and the physical controller.

### Phase 6: Read-only Home Assistant integration

- Implement config flow, zeroconf, pairing, reauthentication, diagnostics, and
  read-only entities.
- Verify push updates occur without entity polling.

### Phase 7: Security gate

- Review authentication, TLS/pinning, token lifecycle, legacy endpoints,
  malformed/slow clients, resource exhaustion, diagnostics, and command design.

Controls cannot proceed until this gate passes.

### Phase 8: Allowlisted controls

- Add power, selected mode, and setpoint entities only.
- Confirm changes through pushed or reconciled reported state.
- Explicitly reject unavailable, disconnected, out-of-range, and unsupported
  operations.

### Phase 9: Hardening and beta

- Long-duration high-rate and reconnect soak tests.
- Multi-client and firmware-upgrade testing.
- HACS packaging, documentation, and staged on-site rollout.

## Acceptance Criteria

- State changes visible to the controller are normally reflected in Home
  Assistant within two seconds.
- A healthy stream does not require fast polling.
- Stream loss falls back to polling and recovers automatically.
- Reboot, dropped messages, and delayed REST responses never regress state.
- A stalled client causes no REST or healthy-stream failures and keeps
  stress-test REST and healthy-stream latency below 1.5 seconds.
- No integration endpoint accepts unauthenticated requests.
- Pairing credentials are never transmitted over plaintext HTTP.
- Home Assistant cannot read or write advanced parameters or raw registers.
- Home Assistant remains optional; removing it changes no controller behavior.
- Control commands never report optimistic state.

## Required Test Coverage

Firmware:

- Shared serializer and revision ordering.
- Strict, missing, invalid, revoked, and rotated token behavior.
- Pairing-window expiry and one-time token delivery.
- WebSocket initial snapshot, telemetry-only changes, reboot, sequence gaps,
  heartbeat, queue overflow, two clients, malformed frames, and stalled sends.
- Web UI and REST responsiveness during push load.
- Heap and socket stability during sustained traffic.

Python client:

- Certificate pinning and fingerprint change.
- Authentication and token rotation.
- Ordering and stale-update rejection.
- Reconnect/backoff, boot changes, malformed frames, cancellation, and cleanup.
- Reconciliation and fallback polling.

Home Assistant:

- Discovery, manual setup, duplicate prevention, reauthentication, and unload.
- Correct selected-mode versus actual-operation mapping.
- Push updates without entity polling.
- Availability, fallback, recovery, command confirmation, and errors.
- Diagnostics redaction.
- Permanent absence of advanced-parameter and generic-register entities.
