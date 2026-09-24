"""HTTP client for the arctic-simulator's semantic API (Macon slave on RS-485).

The simulator is built on the same arctic-macon library as the controller, and
exposes the heat pump's state by *meaning* (named fields, fault codes/sites),
never by register number. This client deliberately has no register or bit
knowledge: the field names, ranges, enum keys and the fault catalog (codes,
labels, severities, sites) all come from the simulator at runtime, so there is
nothing here that can drift out of sync with the library.

Simulator repo: https://github.com/sslivins/arctic-simulator
"""

from typing import Any, Dict, List, Optional

import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry


class SimulatorError(Exception):
    """Raised when the simulator rejects a request."""


class ArcticSimClient:
    def __init__(self, base_url: str = "http://arctic-sim.local", timeout: float = 5.0):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.session = requests.Session()
        # The Atom S3 drops the odd request over WiFi; retry transient failures.
        retry = Retry(total=3, backoff_factor=0.5, allowed_methods=None,
                      status_forcelist=[502, 503, 504])
        self.session.mount("http://", HTTPAdapter(max_retries=retry))
        self._catalog: Optional[List[dict]] = None
        self._fields: Optional[List[dict]] = None

    # -- transport -------------------------------------------------------

    def _req(self, method: str, path: str, body: Any = None) -> dict:
        r = self.session.request(method, f"{self.base_url}{path}", json=body,
                                 timeout=self.timeout)
        if r.status_code >= 400:
            raise SimulatorError(f"{method} {path} failed ({r.status_code}): {r.text}")
        return r.json() if r.content else {}

    # -- identity --------------------------------------------------------

    def get_status(self) -> dict:
        return self._req("GET", "/api/status")

    def is_reachable(self) -> bool:
        try:
            self.get_status()
            return True
        except Exception:
            return False

    def macon_identity(self) -> dict:
        """{"api_version", "layout_fingerprint", "catalog_fingerprint"} of the sim's library."""
        return self.get_status().get("macon") or {}

    def bus_stats(self) -> dict:
        return self.get_status()["tuya_stats"]

    def reboot(self) -> None:
        try:
            self._req("POST", "/api/reboot")
        except requests.RequestException:
            pass  # the connection may drop as it goes down

    # -- semantic state --------------------------------------------------

    def fields(self) -> List[dict]:
        """Field catalog: [{name, kind, unit, min, max, step, keys?, ...}]."""
        if self._fields is None:
            self._fields = self._req("GET", "/api/fields")["fields"]
        return self._fields

    def field(self, name: str) -> dict:
        for f in self.fields():
            if f["name"] == name:
                return f
        raise KeyError(name)

    def state(self) -> dict:
        """{"fields": {...}, "operation", "compressor_running", "faults": [...]}."""
        return self._req("GET", "/api/state")

    def set(self, **fields) -> dict:
        """Atomically set named fields (all-or-nothing). Returns the new state."""
        return self._req("PATCH", "/api/state", fields)

    def load_preset(self, name: str) -> dict:
        """Presets: idle, heating, cooling, hot_water, defrost, fault_p01."""
        return self._req("POST", "/api/preset", {"name": name})

    # -- faults ----------------------------------------------------------

    def fault_catalog(self) -> List[dict]:
        """[{id, code, label, severity, resolution, sites: [{site, label, severity}]}]."""
        if self._catalog is None:
            self._catalog = self._req("GET", "/api/faults/catalog")["faults"]
        return self._catalog

    def active_faults(self) -> List[dict]:
        return self._req("GET", "/api/faults")["faults"]

    def set_fault(self, code: str, active: bool = True) -> dict:
        """Light (or clear) every site of an OEM fault code."""
        return self._req("POST", "/api/faults", {"code": code, "active": active})

    def set_fault_site(self, site: int, active: bool = True) -> dict:
        """Light (or clear) one specific fault site (one bit)."""
        return self._req("POST", "/api/faults", {"site": site, "active": active})

    def clear_faults(self) -> dict:
        return self._req("POST", "/api/faults/clear", {})

    # -- controller writes -----------------------------------------------

    def commands(self) -> dict:
        """Controller fc=0x06 writes received: {"total", "commands": [...]}."""
        return self._req("GET", "/api/commands")

    def commands_since(self, total_before: int) -> List[dict]:
        data = self.commands()
        new = data["total"] - total_before
        return data["commands"][-new:] if new > 0 else []

    # -- bench lease -----------------------------------------------------

    def acquire_lease(self, owner: str, ttl_s: int = 1800) -> dict:
        return self._req("POST", "/api/lease", {"owner": owner, "ttl_s": ttl_s})

    def release_lease(self, owner: Optional[str] = None, force: bool = False) -> Dict:
        body: Dict[str, Any] = {"force": True} if force else {"owner": owner}
        return self._req("DELETE", "/api/lease", body)
