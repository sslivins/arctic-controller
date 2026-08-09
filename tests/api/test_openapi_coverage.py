"""Ensure every registered production HTTP operation is documented."""

import re
from pathlib import Path

import yaml


ROOT = Path(__file__).resolve().parents[2]
API_SERVER = ROOT / "main" / "api_server.cpp"
OPENAPI_SPEC = ROOT / "docs" / "openapi.yaml"

HTTP_METHODS = {"get", "post", "put", "patch", "delete"}
ROUTE_ALIASES = {
    "/api/heatpump/advanced/*": "/api/heatpump/advanced/{id}",
}


def _registered_operations():
    source = API_SERVER.read_text(encoding="utf-8")
    operations = set()
    blocks = re.finditer(
        r"httpd_uri_t\s+\w+\s*=\s*\{(?P<body>.*?)\};",
        source,
        re.DOTALL,
    )

    for match in blocks:
        body = match.group("body")
        uri = re.search(r'\.uri\s*=\s*"([^"]+)"', body)
        method = re.search(
            r"\.method\s*=\s*HTTP_(GET|POST|PUT|PATCH|DELETE)",
            body,
        )
        if not uri or not method:
            continue

        path = ROUTE_ALIASES.get(uri.group(1), uri.group(1))
        if path.startswith("/api/") or path in {"/login", "/logout"}:
            operations.add((path, method.group(1).lower()))

    return operations


def _documented_operations():
    spec = yaml.safe_load(OPENAPI_SPEC.read_text(encoding="utf-8"))
    return {
        (path, method)
        for path, path_item in spec["paths"].items()
        for method in path_item
        if method in HTTP_METHODS
    }


def test_all_registered_operations_are_documented():
    undocumented = _registered_operations() - _documented_operations()

    assert not undocumented, "Undocumented API operations:\n" + "\n".join(
        f"  {method.upper()} {path}" for path, method in sorted(undocumented)
    )
