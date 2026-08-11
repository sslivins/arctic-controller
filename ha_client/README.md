# Arctic Controller async client

This package implements the secure REST and WebSocket client used by the
Home Assistant integration. Each `ArcticControllerClient` instance owns the
credentials, certificate pin, ordering state, reconnect loop, and polling
state for exactly one controller.

The package intentionally has no Home Assistant dependency so its protocol,
recovery, and multi-controller behavior can be tested independently.

```python
from arctic_controller import ArcticControllerClient

client = ArcticControllerClient(
    "192.168.1.21",
    token,
    certificate_fingerprint,
    device_id="arctic-001122334455",
)
client.subscribe(handle_snapshot)
client.subscribe_status(handle_status)
await client.start()
```

Call `await client.stop()` when unloading the owning config entry. Sessions
passed into the constructor remain owned by the caller; sessions created by
the client are closed automatically.
