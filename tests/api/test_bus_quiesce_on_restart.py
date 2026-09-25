"""Static contract: every reboot path hands the RS485 bus back cleanly.

An OTA/reboot must not truncate a heat-pump bus frame mid-wire, leave the
transceiver driver-enable asserted, or let a last-second control write race
the reset. These checks pin the wiring so a refactor cannot silently drop it.
"""
from pathlib import Path

MAIN = Path(__file__).resolve().parents[2] / "main"


def _read(rel):
    return (MAIN / rel).read_text(encoding="utf-8")


def test_safe_restart_quiesces_bus_before_reset():
    src = _read("system_restart.cpp")
    quiesce = src.index("macon_master::quiesce_for_restart(")
    assert quiesce < src.index("esp_rom_software_reset_system();")
    assert quiesce < src.index("spi_flash_op_lock();")


def test_factory_reset_quiesces_bus_before_reset():
    src = _read("factory_reset.cpp")
    assert "macon_master::begin_shutdown();" in src
    assert src.index("macon_master::quiesce_for_restart(") < src.index("esp_restart();")


def test_ota_paths_reject_writes_once_reboot_committed():
    api = _read("api_server.cpp")
    upload_ok = api.index("Firmware upload complete")
    assert api.index("macon_master::begin_shutdown();", upload_ok) < api.index(
        "system_safe_restart();", upload_ok)

    ota = _read("ota_manager.cpp")
    quiesce_net = ota.index("Quiescing network before flashing")
    assert ota.index("macon_master::begin_shutdown();", quiesce_net) < ota.index(
        "esp_ota_begin(", quiesce_net)


def test_control_writes_honour_shutdown():
    src = _read("tuya/macon_master.cpp")
    for fn in ("guarded_setpoint", "set_working_mode", "write_register"):
        body = src[src.index(fn + "("):]
        body = body[:body.index("\n}\n") if "\n}\n" in body else len(body)]
        assert "acquire_for_write(" in body, fn


def test_de_pin_driven_low_first_in_app_main():
    src = _read("main.cpp")
    body = src[src.index('extern "C" void app_main(void)'):]
    first_call = body.index("macon_master::drive_de_low_early();")
    assert first_call < body.index("log_buffer_init();")
