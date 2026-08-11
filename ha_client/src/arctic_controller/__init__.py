"""Async client for the Arctic Heat Pump Controller."""

from .client import (
    ArcticControllerClient,
    SnapshotCallback,
    StatusCallback,
)
from .exceptions import (
    ArcticAuthenticationError,
    ArcticCertificateError,
    ArcticConnectionError,
    ArcticControllerError,
    ArcticPairingError,
    ArcticProtocolError,
)
from .models import (
    PROTOCOL_VERSION,
    ClientStatus,
    ComponentState,
    ControllerCapabilities,
    ControllerState,
    ErrorState,
    PairingResult,
    ReadingState,
    SetpointRange,
    SetpointState,
    StateSnapshot,
    TemperatureState,
)

__all__ = [
    "PROTOCOL_VERSION",
    "ArcticAuthenticationError",
    "ArcticCertificateError",
    "ArcticConnectionError",
    "ArcticControllerClient",
    "ArcticControllerError",
    "ArcticPairingError",
    "ArcticProtocolError",
    "ComponentState",
    "ClientStatus",
    "ControllerCapabilities",
    "ControllerState",
    "ErrorState",
    "PairingResult",
    "ReadingState",
    "SetpointRange",
    "SetpointState",
    "SnapshotCallback",
    "StateSnapshot",
    "StatusCallback",
    "TemperatureState",
]
