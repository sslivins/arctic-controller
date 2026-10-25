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


def test_socket_budget_covers_all_three_http_servers() -> None:
    defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")
    sdkconfig = (ROOT / "sdkconfig").read_text(encoding="utf-8")
    api = (ROOT / "main" / "api_server.cpp").read_text(encoding="utf-8")

    assert "CONFIG_LWIP_MAX_SOCKETS=24" in defaults
    assert "CONFIG_LWIP_MAX_SOCKETS=24" in sdkconfig
    # Port 80 is the full web server in HTTP mode and a health-only stub in
    # HTTPS mode; only one of 80/443 is ever full-size.
    assert "http_config.max_open_sockets   = full ? 7 : 2;" in api
    assert "start_http_locked(!s_https_mode)" in api
    assert "ssl_config.httpd.max_open_sockets   = 7;" in api
    assert "integration_config.httpd.max_open_sockets = 5;" in api
    assert "websocket_config.max_open_sockets = 2;" in api


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
    pairing = (ROOT / "main" / "setup_pairing.cpp").read_text(
        encoding="utf-8"
    )
    auth_header = (ROOT / "main" / "auth_manager.h").read_text(
        encoding="utf-8"
    )

    # The factory sign-in is detected by hashing the factory password, never
    # by comparing the username. The password itself now lives in a single
    # named constant, so pin the constant's value as well -- otherwise the
    # indirection would let the factory password change without this contract
    # noticing.
    assert '#define AUTH_FACTORY_PASSWORD "arctic"' in auth_header
    assert "hash_password(AUTH_FACTORY_PASSWORD, default_hash)" in auth
    assert 'strcmp(state.username, "arctic")' not in auth
    assert "strcmp(state.username, AUTH_FACTORY_USERNAME)" not in auth
    assert "setup_pairing_authorize(code)" in api
    # A certificate that will not serve falls back to HTTP, never to no UI.
    assert "Failed to start HTTPS server: %s -- falling back to HTTP" in api
    assert "setup_pairing_authorize" in pairing


def test_https_mode_follows_administrator_certificate() -> None:
    """Port 443 runs only with an administrator certificate, never the identity.

    The self-signed identity is reserved for the Home Assistant server on 8443
    (which pins it); serving the web UI with it made every browser warn. With
    an administrator certificate installed, plaintext must not fall back to the
    full UI, and certificate changes must restart into the new mode.
    """
    api = (ROOT / "main" / "api_server.cpp").read_text(encoding="utf-8")
    start = api[api.index("static bool api_server_start_locked(void)\n{"):]
    start = start[:start.index("\n}\n")]
    https = start[start.index("if (s_https_mode) {"):start.index("// ---- Port 80 ----")]

    assert "s_https_mode = tls_mgr_has_certs();" in start
    assert "tls_mgr_get_cert(&cert_len)" in https
    assert "tls_mgr_get_identity_cert" not in https
    assert "tls_mgr_get_identity_cert(&cert_len)" in api[
        api.index("integration_config.port_secure = 8443;") - 600:
        api.index("integration_config.port_secure = 8443;")
    ]
    assert "s_https_mode ? server_ssl != NULL : server != NULL" in api
    assert "tls_mgr_key_matches_cert" in api
    post = api[api.index("static esp_err_t tls_cert_post_handler(httpd_req_t* req)\n{"):]
    post = post[:post.index("\n}\n")]
    delete = api[api.index("static esp_err_t tls_cert_delete_handler(httpd_req_t* req)\n{"):]
    delete = delete[:delete.index("\n}")]
    assert 'send_tls_change_response_and_restart(' in post
    assert '"https"' in post
    assert 'send_tls_change_response_and_restart(' in delete
    assert '"http"' in delete


def test_credential_change_without_a_session_requires_a_one_time_code() -> None:
    """A sessionless caller must never be able to set credentials.

    /api/auth/credentials deliberately accepts "valid session OR one-time
    code" so a locked-out owner can recover, which means the code check is
    the only thing standing between an unauthenticated LAN client and a
    controller takeover. Pin the gate so the ``|| !has_session`` half cannot
    be dropped -- without it, recovery would become an open endpoint.
    """
    api = (ROOT / "main" / "api_server.cpp").read_text(encoding="utf-8")

    handler = api[
        api.index("static esp_err_t auth_credentials_post_handler(httpd_req_t* req)\n{"):
    ]
    handler = handler[: handler.index("\n}\n")]

    assert "const bool has_session = check_web_auth(req);" in handler
    assert (
        "if (auth_mgr_credentials_change_required() || !has_session) {"
        in handler
    )
    assert "setup_pairing_authorize(code)" in handler
    # The code must be verified before the credentials are written, not after.
    assert handler.index("setup_pairing_authorize(code)") < handler.index(
        "auth_mgr_set_credentials"
    )


def test_login_is_throttled_after_repeated_failures() -> None:
    """Short passwords are only safe because online guessing is throttled.

    AUTH_MIN_PASSWORD_LEN is 8 on the explicit assumption that /login applies
    an exponential cooldown. If the cooldown is ever removed, the minimum
    length has to be revisited at the same time -- so tie them together here.
    """
    api = (ROOT / "main" / "api_server.cpp").read_text(encoding="utf-8")
    auth_header = (ROOT / "main" / "auth_manager.h").read_text(encoding="utf-8")

    assert "#define AUTH_MIN_PASSWORD_LEN 8" in auth_header
    assert "LOGIN_FAILURES_BEFORE_COOLDOWN" in api
    assert "login_note_failure()" in api
    assert "login_note_success()" in api

    handler = api[api.index("static esp_err_t web_login_handler(httpd_req_t* req)\n{"):]
    handler = handler[: handler.index("\n}\n")]
    assert "login_cooldown_remaining()" in handler
    assert "429 Too Many Requests" in handler
    assert "Retry-After" in handler


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
    assert "heating_setpoint = demo_controls || active_master_controls" in capabilities


def test_home_assistant_command_validation_rejects_ambiguous_controls() -> None:
    api = (ROOT / "main" / "api_server.cpp").read_text(encoding="utf-8")

    assert "command_id was already used with a different command" in api
    assert "Setpoint is outside the advertised inclusive range" in api
    assert "Power control is unsupported by the active runtime" in api
    assert "Selected-mode control is unsupported by the active runtime" in api
    assert '"heating") == 0' in api
    assert "parseSelectableWorkingMode" in api
    types = (ROOT / "main" / "heatpump_types.h").read_text(encoding="utf-8")
    assert '{"hot_water_cooling", WorkingMode::HOT_WATER_COOLING}' in types
    assert "freeze_protection" not in types.split("parseSelectableWorkingMode", 1)[1]
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


def test_device_tests_cover_http_then_https() -> None:
    """The bench checks HTTP mode, the certificate upload and HTTPS mode once,
    before the suites, rather than running every suite on both schemes."""
    workflow = (
        ROOT / ".github" / "workflows" / "device-tests.yml"
    ).read_text(encoding="utf-8")
    transition = (
        ROOT / ".github" / "scripts" / "tls_mode.py"
    ).read_text(encoding="utf-8")

    readiness = workflow.index("- name: Require HTTP-mode readiness")
    upgrade = workflow.index("- name: Upgrade migration check")
    step = workflow.index("python .github/scripts/tls_mode.py to-https")
    suites = workflow.index("- name: Run device UI tests")
    wifi = workflow.index("- name: Run Wi-Fi recovery tests")
    revert = workflow.index("python .github/scripts/tls_mode.py to-http\n")
    reset = workflow.index("- name: Reset device to baseline TLS/auth state")
    assert readiness < upgrade < step < suites < wifi < revert < reset
    # Port 443 must be closed in HTTP mode.
    assert "require_https_closed" in workflow[readiness:upgrade]
    # The transition must be able to fail the job.
    assert "continue-on-error:" not in workflow[workflow.index("- name: HTTP to HTTPS"):step]
    revert_step = workflow[workflow.index("- name: HTTPS to HTTP"):revert]
    assert "continue-on-error:" not in revert_step
    assert "if: always() && steps.tls_transition.outcome == 'success'" in revert_step

    # A throwaway certificate, so no real private key travels over HTTP.
    assert "generate_cert(" in transition
    assert "TLS_PRIVKEY_PEM" not in transition
    assert "TLS_PRIVKEY_PEM" not in workflow
    for check in (
        "port 443 is closed",
        "the dashboard is served over HTTP",
        "the controller restarts into HTTPS",
        "is a 404, not a redirect",
        "the API is not served over HTTP",
        "port 443 presents the uploaded certificate",
        "the authenticated API answers over HTTPS",
        "the controller restarts into HTTP",
        "the controller restarted",
    ):
        assert check in transition
    assert "ARCTIC_ADMIN_CERT_FILE" in transition
