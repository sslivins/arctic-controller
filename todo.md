# Arctic Controller – TODO

## COP / Energy Monitoring

- [ ] Add configurable water flow rate setting (default ~20 L/min, user adjusts to match their circulator pump)
- [ ] Store flow rate in NVS so it persists across reboots
- [ ] Start polling saturation temp registers (2111-2113) — currently skipped in `pollTemperatures()`
- [ ] Calculate thermal output: `flow_rate (L/s) × 4186 (J/kg·K) × ΔT (outlet - inlet)`
- [ ] Calculate COP: `thermal_output_W / electrical_input_W` (only when compressor running)
- [ ] Expose COP, thermal output, electrical input in `HeatPumpState` struct
- [ ] Add COP/energy fields to REST API and demo mode injection
- [ ] Add flow rate configuration to the Advanced/Params screen

## Main Screen Layout Redesign

- [ ] Rethink layout to maximize useful information at a glance
- [ ] Surface key temperatures (inlet, outlet, ambient, coil) without needing to tap into Temps screen
- [ ] Add energy section: Power In (W), Heat Out (W), COP (×) — prominent, color-coded
- [ ] Reduce dead space (~500px spacer currently wasted)
- [ ] Compact the controls (mode + setpoint in one row)
- [ ] Add setpoint delta to hero tank temperature display
