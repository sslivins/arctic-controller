"""Constants for the Arctic Controller integration."""

from homeassistant.const import Platform

DOMAIN = "arctic_controller"

CONF_DEVICE_ID = "device_id"
CONF_FINGERPRINT = "fingerprint"
CONF_TOKEN = "token"
DEFAULT_PORT = 8443

PLATFORMS = [
    Platform.BINARY_SENSOR,
    Platform.CLIMATE,
    Platform.SENSOR,
]
