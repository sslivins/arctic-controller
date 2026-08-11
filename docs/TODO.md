# Arctic Controller - TODO

## Home Assistant Local Integration

Architecture and security requirements are defined in
[`home-assistant-integration.md`](home-assistant-integration.md).

- [x] Define hybrid REST + WebSocket + reconciliation architecture
- [x] Define threat model and explicit control allowlist
- [x] Run the WSS socket/heap/slow-client feasibility spike
- [x] Implement integration identity, pairing, strict auth, and versioned state
- [x] Implement resilient device-hosted WebSocket push
- [x] Build async Python client and read-only Home Assistant integration
- [x] Complete security gate before enabling controls
- [x] Add only power, selected mode, and setpoint controls
- [ ] Complete soak testing and staged beta rollout

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
- [x] `GET /api/heatpump/errors` - Decoded error codes with severity and history
- [ ] `GET /api/system/info` - Firmware version, WiFi, uptime
- [ ] `GET /api/settings` - App preferences
- [ ] `PUT /api/settings` - Update preferences

## Error Status Display
- [x] Parse error registers (2137, 2138) into human-readable messages
- [x] UI: Error indicator on main screen when errors present
- [x] UI: Error details screen showing active errors with descriptions
- [x] Map Arctic error codes (E01, P02, r01, FA, etc.) to register bits
- [x] Resolution text for each error code (expandable on tap)
- [x] Duration and timestamp tracking (12/24h format from settings)
- [x] Visual distinction: active errors (dark red) vs cleared errors (dimmed)
- [x] Error history limited to 10 entries
- [x] API: Include decoded error list in `/api/heatpump/status`
- [x] API: `GET /api/heatpump/errors` - Dedicated endpoint with error history
- [x] API: Demo mode returns sample errors (1 active + 2 cleared)
- [x] Store error history with timestamps (ring buffer, 10 entries)
- [x] OpenAPI spec updated with resolution, occurred fields; removed register/raw

## Localization (i18n)
- [x] i18n infrastructure in place (`i18n.h`, `strings.h`, `i18n.cpp`)
- [x] Add strings for all heat pump UI elements (106 string IDs)
- [x] Translate to French and Spanish (all 106 strings)
- [x] Latin-extended fonts already in use for i18n support
- [x] All 5 heat pump screens converted to `i18n_get()` calls
- [x] P-parameter names translated (25 params, EN/FR/ES)
- [x] P-parameter category headers translated (EEV, Defrost, Protection, Auto Mode, Pump & Valve)
- [x] Range format string in edit dialogs translated
- [x] Settings menu refreshes on back-navigation to pick up language changes
- [x] Language screen title updates immediately on language change
- [ ] Localize error resolution text (currently English-only from Arctic docs)
- [ ] Localize P-parameter descriptions (currently English technical text)
- [ ] Localize settings menu "Demo Mode" and "Temperature" row labels

## Web Interface
- [x] Dashboard with power toggle, mode selector, component status indicators
- [x] Temperature display for all 9 sensors (tank, outlet, inlet, outdoor, discharge, suction, coils, IPM)
- [x] Setpoint controls with ± adjustment buttons (cooling, heating, hot water)
- [x] System readings (compressor freq, fan RPM, voltages, pressures, EEV, power)
- [x] Active errors with severity badges, resolution text, timestamps
- [x] Error history with occurred/cleared timestamps
- [x] P-parameter editor page with category grouping and inline editing
- [x] 3-tab navigation (Dashboard, Parameters, Settings)
- [x] Full i18n (EN/FR/ES) for all new UI elements (~55 new translation keys)
- [x] Mobile-responsive design (grid breakpoints for all new components)
- [ ] Settings page (demo mode toggle, temp units via web)

## Time Settings
- [x] Fixed duplicate time_screen.cpp conflict (removed old file from CMakeLists)
- [x] 12/24h format now syncs via `time_mgr_set/get_24h_format()`
- [x] Timezone panel overlap fixed (flex layout)
- [x] Rounded corners on timezone roller

## UI Polish
- [x] Fade animations (300ms) on all heat pump sub-screens
- [x] Larger fonts (32/24px) on error cards
- [x] Removed snake_case names from error display
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
