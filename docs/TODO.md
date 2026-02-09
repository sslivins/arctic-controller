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
All temperatures in Celsius (native units). snake_case naming.

### Read Endpoints
- [x] `GET /api/heatpump/status` - All readings (temps, pressures, status, setpoints, power)
- [x] `GET /api/heatpump/params` - All P-parameters with descriptive keys
- [x] `GET /api/heatpump/params/:id` - Single parameter (accepts key or P-code)

### Write Endpoints
- [x] `PUT /api/heatpump/power` - `{ "on": true/false }`
- [x] `PUT /api/heatpump/mode` - `{ "mode": "floor_heating" }` (cooling/floor_heating/fan_coil_heating/hot_water/auto)
- [x] `PUT /api/heatpump/setpoints` - `{ "cooling": 18, "heating": 45, "hot_water": 50 }`
- [x] `PUT /api/heatpump/params/:id` - Plain integer body (e.g., `25`), accepts key or P-code

### Parameter Format
```json
{
  "water_inlet_low_limit": {
    "value": 5,
    "p_code": "P01",
    "unit": "°C",
    "min": 3,
    "max": 15
  }
}
```

### Future/Optional
- [ ] `GET /api/heatpump/errors` - Decoded error codes
- [ ] `GET /api/system/info` - Firmware version, WiFi, uptime
- [ ] `GET /api/settings` - App preferences
- [ ] `PUT /api/settings` - Update preferences

## Error Status Display
- [ ] Parse error registers (2137, 2138) into human-readable messages
- [ ] UI: Error indicator on main screen when errors present
- [ ] UI: Error details screen showing active errors with descriptions
- [ ] API: Include decoded error list in `/api/heatpump/status`
- [ ] API: `GET /api/heatpump/errors` - Dedicated endpoint with error history
- [ ] Store error history with timestamps (ring buffer)

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

## Event Viewer / Logging
- [ ] Log heat pump events with timestamps (compressor start/stop, fan speed changes, pump on/off, errors)
- [ ] In-memory ring buffer for recent events
- [ ] UI screen to view event history
- [ ] `GET /api/heatpump/events` - API endpoint for event log
- [ ] Persist events to flash/SD (optional)

---
*Last updated: Feb 9, 2026*
