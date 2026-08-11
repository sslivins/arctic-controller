"""Exceptions raised by the Arctic Controller client."""


class ArcticControllerError(Exception):
    """Base class for Arctic Controller client errors."""


class ArcticConnectionError(ArcticControllerError):
    """The controller could not be reached or returned an HTTP error."""


class ArcticCertificateError(ArcticConnectionError):
    """The controller certificate does not match the configured pin."""


class ArcticAuthenticationError(ArcticControllerError):
    """The integration credential was rejected."""


class ArcticPairingError(ArcticControllerError):
    """The physical pairing claim was rejected."""


class ArcticProtocolError(ArcticControllerError):
    """The controller returned an invalid or unsupported protocol message."""
