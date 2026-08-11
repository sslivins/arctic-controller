"""Static security contracts for production Home Assistant support."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_production_configuration_disables_test_endpoints() -> None:
    kconfig = (ROOT / "main" / "Kconfig.projbuild").read_text(
        encoding="utf-8"
    )
    defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")
    sdkconfig = (ROOT / "sdkconfig").read_text(encoding="utf-8")
    release = (
        ROOT / ".github" / "workflows" / "create-release.yml"
    ).read_text(encoding="utf-8")

    assert "config TEST_ENDPOINTS" in kconfig
    assert "default n" in kconfig
    assert "CONFIG_TEST_ENDPOINTS=n" in defaults
    assert "CONFIG_TEST_ENDPOINTS=y" not in sdkconfig
    assert "Verify production security configuration" in release


def test_integration_credential_changes_use_transaction_mutex() -> None:
    source = (ROOT / "main" / "auth_manager.cpp").read_text(
        encoding="utf-8"
    )

    assert "integration_token_mutex" in source
    issue = source[
        source.index("bool auth_mgr_issue_integration_token"):
        source.index("bool auth_mgr_revoke_integration_token")
    ]
    revoke = source[
        source.index("bool auth_mgr_revoke_integration_token"):
        source.index("bool auth_mgr_validate_integration_token")
    ]
    for operation in (issue, revoke):
        assert "xSemaphoreTake(integration_token_mutex" in operation
        assert "nvs_commit" in operation
        assert "xSemaphoreGive(integration_token_mutex)" in operation


def test_legacy_remote_api_fails_closed() -> None:
    auth = (ROOT / "main" / "auth_manager.cpp").read_text(
        encoding="utf-8"
    )
    api = (ROOT / "main" / "api_server.cpp").read_text(
        encoding="utf-8"
    )

    assert "Ignoring attempt to disable mandatory web authentication" in auth
    assert "Ignoring attempt to disable mandatory API authentication" in auth
    assert "credentials_change_required" in api
    check_api = api[
        api.index("static bool check_api_auth"):
        api.index("static bool check_integration_auth")
    ]
    assert "auth_mgr_credentials_change_required()" in check_api
    assert "Auth disabled" not in check_api
    assert "allow access from the web interface" not in check_api


def test_factory_credential_replacement_requires_physical_tls_flow() -> None:
    auth = (ROOT / "main" / "auth_manager.cpp").read_text(
        encoding="utf-8"
    )
    api = (ROOT / "main" / "api_server.cpp").read_text(
        encoding="utf-8"
    )
    pairing = (ROOT / "main" / "ha_pairing.cpp").read_text(
        encoding="utf-8"
    )

    assert 'hash_password("arctic", default_hash)' in auth
    assert 'strcmp(state.username, "arctic")' not in auth
    assert "ha_pairing_authorize(code)" in api
    assert "persistent device identity" in api
    assert "Failed to start mandatory HTTPS server" in api
    assert "ha_pairing_authorize" in pairing


def test_device_test_build_explicitly_overrides_production_config() -> None:
    workflow = (
        ROOT / ".github" / "workflows" / "device-tests.yml"
    ).read_text(encoding="utf-8")

    assert 'disabled = "# CONFIG_TEST_ENDPOINTS is not set"' in workflow
    assert "CONFIG_TEST_ENDPOINTS=y" in workflow
    assert "Verify test instrumentation was compiled" in workflow


def test_home_assistant_controls_are_allowlisted_and_generation_guarded() -> None:
    api = (ROOT / "main" / "api_server.cpp").read_text(encoding="utf-8")
    auth = (ROOT / "main" / "auth_manager.cpp").read_text(encoding="utf-8")
    capabilities = (
        ROOT / "main" / "ha_integration.cpp"
    ).read_text(encoding="utf-8")

    assert '"/api/v1/control/power"' in api
    assert '"/api/v1/control/mode"' in api
    assert '"/api/v1/control/setpoint"' in api
    assert "ha_command_begin" in api
    assert api.count("auth_mgr_begin_control_write") >= 3
    assert "auth_mgr_end_control_write" in api
    assert "auth_mgr_begin_control_write" in auth
    assert "state.integration_generation == generation" in auth
    assert "return state.connected && arctic::isDemoMode();" in api
    assert "macon_master::is_active()" in api
    assert "supported_modes" in capabilities
    assert "setpoint_controls" in capabilities
    assert "Heating setpoint writes are available only" in capabilities


def test_home_assistant_command_validation_rejects_ambiguous_controls() -> None:
    api = (ROOT / "main" / "api_server.cpp").read_text(encoding="utf-8")

    assert "command_id was already used with a different command" in api
    assert "Setpoint is outside the advertised inclusive range" in api
    assert "Power control is unsupported by the active runtime" in api
    assert "Selected-mode control is unsupported by the active runtime" in api
    assert "Heating setpoint is unsupported by the active Tuya runtime" in api
    assert '"heating") == 0' in api
    assert '"floor_heating") == 0' in api
    assert '"fan_coil_heating") == 0' in api
    assert "/api/v1/control/register" not in api
    assert "/api/v1/control/advanced" not in api
    capabilities = (
        ROOT / "main" / "ha_integration.cpp"
    ).read_text(encoding="utf-8")
    assert '"advanced_parameters"' in capabilities


def test_secret_bearing_automation_is_https_only() -> None:
    renewal = (
        ROOT / ".github" / "workflows" / "renew-tls-cert.yml"
    ).read_text(encoding="utf-8")
    device_tests = (
        ROOT / ".github" / "workflows" / "device-tests.yml"
    ).read_text(encoding="utf-8")
    api = (ROOT / "main" / "api_server.cpp").read_text(
        encoding="utf-8"
    )

    assert "ARCTIC_URL: https://" in renewal
    assert "curl -sk" not in renewal
    assert 'for URL in "$ARCTIC_URL" "$ARCTIC_URL_HTTPS"' not in renewal
    assert "steps.read_certs.outputs.privkey" not in renewal
    assert "privkey<<KEY_EOF" not in renewal
    assert 'for URL in "$ARCTIC_URL" "$ARCTIC_URL_HTTPS"' not in device_tests
    assert "http_to_https_redirect_handler" not in api
