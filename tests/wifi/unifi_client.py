"""
Minimal client for the UniFi Network controller, used to knock the bench
controller off Wi-Fi from the outside the way a real access point would.

Authenticates with a UniFi API key (X-API-KEY header), which works for the
classic /proxy/network/api/s/<site>/... endpoints on UniFi OS consoles.
"""

import time

import requests
import urllib3

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)


class UnifiClient:
    def __init__(self, base_url: str, api_key: str, site: str = "default", timeout: float = 10.0):
        self.base_url = base_url.rstrip("/")
        self.site = site
        self.timeout = timeout
        self.session = requests.Session()
        self.session.verify = False  # consoles ship a self-signed certificate
        self.session.headers["X-API-KEY"] = api_key

    def _url(self, path: str) -> str:
        return f"{self.base_url}/proxy/network/api/s/{self.site}/{path}"

    def clients(self) -> list[dict]:
        """Currently associated clients (mac, ip, hostname, ap_mac, signal, ...)."""
        r = self.session.get(self._url("stat/sta"), timeout=self.timeout)
        r.raise_for_status()
        return r.json().get("data", [])

    def find_mac_by_ip(self, ip: str) -> str | None:
        return next((c["mac"] for c in self.clients() if c.get("ip") == ip), None)

    def _stamgr(self, cmd: str, mac: str) -> None:
        r = self.session.post(self._url("cmd/stamgr"), json={"cmd": cmd, "mac": mac.lower()},
                              timeout=self.timeout)
        r.raise_for_status()
        rc = r.json().get("meta", {}).get("rc")
        if rc != "ok":
            raise RuntimeError(f"UniFi {cmd} {mac} failed: {r.text[:200]}")

    def kick(self, mac: str) -> None:
        """Disconnect the client once; it may reassociate immediately."""
        self._stamgr("kick-sta", mac)

    def block(self, mac: str) -> None:
        """Disconnect the client and refuse it until unblock(). Persistent!"""
        self._stamgr("block-sta", mac)

    def unblock(self, mac: str, attempts: int = 5) -> None:
        """Lift a block, retrying: a client left blocked takes the bench offline."""
        last = None
        for i in range(attempts):
            try:
                self._stamgr("unblock-sta", mac)
                return
            except Exception as e:  # noqa: BLE001 - retry anything, re-raise the last
                last = e
                time.sleep(2 * (i + 1))
        raise RuntimeError(f"Could not unblock {mac} after {attempts} attempts: {last}")
