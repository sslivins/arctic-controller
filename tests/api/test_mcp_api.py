"""
Functional tests for MCP (Model Context Protocol) endpoint.

Tests tools/list response structure, scope annotations, and basic
JSON-RPC functionality.

Prerequisites:
  - Device reachable at ARCTIC_URL (default http://arctic.local)
  - ARCTIC_API_KEY env var set
"""

import os
import time

import pytest
import requests
from pathlib import Path
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

# Load .env from repo root if present (local dev)
_env_file = Path(__file__).resolve().parent.parent.parent / ".env"
if _env_file.exists():
    from dotenv import load_dotenv
    load_dotenv(_env_file)

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
API_KEY = os.environ.get("ARCTIC_API_KEY")

# Expected OAuth scopes that should appear in tool annotations
EXPECTED_SCOPES = {
    "arctic:status",
    "arctic:control",
    "arctic:config",
    "arctic:params",
    "arctic:admin",
    "arctic:mcp",
}

# Tools and their expected scope requirements
EXPECTED_TOOL_SCOPES = {
    "get_heatpump_status": ["arctic:status"],
    "get_device_info": ["arctic:status"],
    "get_heatpump_errors": ["arctic:status"],
    "get_event_log": ["arctic:status"],
    "set_heatpump_power": ["arctic:control"],
    "set_heatpump_mode": ["arctic:control"],
    "set_temperature_setpoint": ["arctic:control"],
    "get_parameters": ["arctic:params"],
    "set_parameter": ["arctic:params"],
    "get_wifi_status": ["arctic:config"],
    "get_system_logs": ["arctic:admin"],
    "reboot_device": ["arctic:admin"],
}

# Retry-enabled session for all API calls
_session = requests.Session()
_retry = Retry(total=3, backoff_factor=1, allowed_methods=None,
               status_forcelist=[502, 503, 504])
_session.mount("http://", HTTPAdapter(max_retries=_retry))
_session.mount("https://", HTTPAdapter(max_retries=_retry))


def _mcp_request(method, params=None, request_id=1):
    """Send a JSON-RPC request to the MCP endpoint."""
    payload = {
        "jsonrpc": "2.0",
        "id": request_id,
        "method": method,
    }
    if params is not None:
        payload["params"] = params
    
    headers = {"Content-Type": "application/json"}
    if API_KEY:
        headers["X-API-Key"] = API_KEY
    
    return _session.post(
        f"{BASE_URL}/mcp",
        json=payload,
        headers=headers,
        timeout=10
    )


def _mcp_initialize():
    """Initialize MCP session."""
    return _mcp_request("initialize", {
        "protocolVersion": "2025-03-26",
        "capabilities": {},
        "clientInfo": {"name": "pytest", "version": "1.0"}
    })


@pytest.fixture(scope="module", autouse=True)
def _check_prerequisites():
    """Verify device is reachable and API key is set."""
    if not API_KEY:
        pytest.skip("ARCTIC_API_KEY not set")
    last_err = None
    for attempt in range(3):
        try:
            r = requests.get(f"{BASE_URL}/api/health", timeout=5)
            r.raise_for_status()
            return
        except Exception as e:
            last_err = e
            if attempt < 2:
                time.sleep(2)
    pytest.skip(f"Device unreachable at {BASE_URL}: {last_err}")


@pytest.fixture(scope="module")
def mcp_session():
    """Initialize MCP session once for all tests."""
    r = _mcp_initialize()
    assert r.status_code == 200
    data = r.json()
    assert "result" in data
    return data["result"]


# ═══════════════════════════════════════════════════════════════════════════
# MCP tools/list
# ═══════════════════════════════════════════════════════════════════════════

class TestMCPToolsList:
    """Tests for MCP tools/list method."""
    
    def test_tools_list_returns_200(self, mcp_session):
        """tools/list returns HTTP 200."""
        r = _mcp_request("tools/list")
        assert r.status_code == 200
    
    def test_tools_list_jsonrpc_response(self, mcp_session):
        """tools/list returns valid JSON-RPC response."""
        r = _mcp_request("tools/list", request_id=42)
        data = r.json()
        
        assert data.get("jsonrpc") == "2.0"
        assert data.get("id") == 42
        assert "result" in data
    
    def test_tools_list_has_tools_array(self, mcp_session):
        """tools/list result contains tools array."""
        r = _mcp_request("tools/list")
        data = r.json()
        
        assert "tools" in data["result"]
        assert isinstance(data["result"]["tools"], list)
        assert len(data["result"]["tools"]) > 0
    
    def test_tool_has_required_properties(self, mcp_session):
        """Each tool has name, description, and inputSchema."""
        r = _mcp_request("tools/list")
        tools = r.json()["result"]["tools"]
        
        for tool in tools:
            assert "name" in tool, f"Tool missing name: {tool}"
            assert "description" in tool, f"Tool missing description: {tool}"
            assert "inputSchema" in tool, f"Tool missing inputSchema: {tool}"
    
    def test_all_expected_tools_present(self, mcp_session):
        """All expected tools are listed."""
        r = _mcp_request("tools/list")
        tools = r.json()["result"]["tools"]
        tool_names = {t["name"] for t in tools}
        
        for expected_name in EXPECTED_TOOL_SCOPES.keys():
            assert expected_name in tool_names, f"Missing tool: {expected_name}"
    
    def test_tools_have_required_scopes_annotation(self, mcp_session):
        """Tools with scope requirements have requiredScopes annotation."""
        r = _mcp_request("tools/list")
        tools = r.json()["result"]["tools"]
        
        tools_with_annotations = 0
        for tool in tools:
            if "annotations" in tool and "requiredScopes" in tool.get("annotations", {}):
                tools_with_annotations += 1
                scopes = tool["annotations"]["requiredScopes"]
                assert isinstance(scopes, list), f"requiredScopes should be array: {tool['name']}"
                assert len(scopes) > 0, f"requiredScopes should not be empty: {tool['name']}"
        
        # At least some tools should have scope annotations
        assert tools_with_annotations > 0, "No tools have requiredScopes annotations"
    
    def test_scope_annotations_are_valid_scopes(self, mcp_session):
        """All scope annotations use valid scope names."""
        r = _mcp_request("tools/list")
        tools = r.json()["result"]["tools"]
        
        for tool in tools:
            annotations = tool.get("annotations", {})
            scopes = annotations.get("requiredScopes", [])
            for scope in scopes:
                assert scope in EXPECTED_SCOPES, \
                    f"Unknown scope '{scope}' in tool '{tool['name']}'"
    
    def test_expected_tool_scopes_match(self, mcp_session):
        """Specific tools have expected scope requirements."""
        r = _mcp_request("tools/list")
        tools = r.json()["result"]["tools"]
        tools_by_name = {t["name"]: t for t in tools}
        
        for tool_name, expected_scopes in EXPECTED_TOOL_SCOPES.items():
            if tool_name in tools_by_name:
                tool = tools_by_name[tool_name]
                annotations = tool.get("annotations", {})
                actual_scopes = annotations.get("requiredScopes", [])
                
                for scope in expected_scopes:
                    assert scope in actual_scopes, \
                        f"Tool '{tool_name}' missing expected scope '{scope}'"


# ═══════════════════════════════════════════════════════════════════════════
# MCP initialization
# ═══════════════════════════════════════════════════════════════════════════

class TestMCPInitialize:
    """Tests for MCP initialize method."""
    
    def test_initialize_returns_200(self):
        """initialize returns HTTP 200."""
        r = _mcp_initialize()
        assert r.status_code == 200
    
    def test_initialize_returns_capabilities(self):
        """initialize returns server capabilities."""
        r = _mcp_initialize()
        data = r.json()
        
        assert "result" in data
        assert "capabilities" in data["result"]
    
    def test_initialize_returns_server_info(self):
        """initialize returns server info with name and version."""
        r = _mcp_initialize()
        data = r.json()
        
        result = data["result"]
        assert "serverInfo" in result
        assert "name" in result["serverInfo"]
        assert "version" in result["serverInfo"]
    
    def test_initialize_returns_protocol_version(self):
        """initialize returns protocol version."""
        r = _mcp_initialize()
        data = r.json()
        
        assert "protocolVersion" in data["result"]


# ═══════════════════════════════════════════════════════════════════════════
# MCP authentication
# ═══════════════════════════════════════════════════════════════════════════

class TestMCPAuth:
    """Tests for MCP authentication requirements."""
    
    def test_mcp_without_auth(self):
        """MCP endpoint behavior without authentication."""
        r = requests.post(
            f"{BASE_URL}/mcp",
            json={"jsonrpc": "2.0", "id": 1, "method": "tools/list"},
            headers={"Content-Type": "application/json"},
            timeout=10
        )
        # Depending on auth config, may return 200 or 401
        assert r.status_code in (200, 401)
