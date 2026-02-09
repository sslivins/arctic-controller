# Arctic Controller - TODO

## Fahrenheit Support (Complete)
- [x] Basic C→F conversion for display (`app_prefs_convert_temp()`)
- [x] Temperature unit toggle in settings menu
- [x] Add `ParamUnit` enum with `TEMP_ABSOLUTE` and `TEMP_OFFSET` types
  - Absolute temps: `(C × 9/5) + 32`
  - Differential/offset temps (P32, P39, P42): `C × 9/5` (no +32)
- [x] Float-based conversion functions for F editing precision
  - `app_prefs_convert_temp_f()`, `app_prefs_convert_temp_diff_f()`
  - `app_prefs_temp_to_celsius_from_f()`, `app_prefs_temp_diff_to_celsius_from_f()`
- [x] All parameter definitions updated with proper unit types
- [x] Edit dialog uses float internally, displays rounded, converts back to C on save
- [x] Offset parameters show "Δ°F" notation via `app_prefs_temp_diff_unit_str()`

## API for Heat Pump Values
- [ ] REST endpoint: GET /api/heatpump/state (all current readings)
- [ ] REST endpoint: GET /api/heatpump/params (P-parameters)
- [ ] REST endpoint: PUT /api/heatpump/params/:id (write P-parameter)
- [ ] REST endpoint: PUT /api/heatpump/setpoints (write setpoints)
- [ ] REST endpoint: PUT /api/heatpump/power (on/off)
- [ ] REST endpoint: PUT /api/heatpump/mode (working mode)

## Localization (i18n)
- [x] i18n infrastructure in place (`i18n.h`)
- [ ] Add strings for all heat pump UI elements
- [ ] Translate to additional languages (French, Spanish, etc.)
- [ ] Latin-extended fonts already in use for i18n support

## Web Interface
- [ ] Dashboard page showing heat pump status
- [ ] Temperature/setpoint controls
- [ ] P-parameter editor
- [ ] Settings page (demo mode, temp units, etc.)
- [ ] Mobile-responsive design

## Minor UI Items
- [ ] Status bar fonts - migrate to `ui_common.h` Latin-extended fonts
- [ ] Startup animation fonts - use Latin-extended fonts
- [ ] More prominent connection status indicator on main screen?

---
*Last updated: Feb 8, 2026*
