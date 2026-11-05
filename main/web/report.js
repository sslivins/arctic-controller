// Heat pump activity report. Runs inside the saved HTML file: DATA (built by
// buildReportData in index.html) and the tschart library come before it.
(() => {
const M = DATA.meta, TZ = M.tz || 'UTC';
document.head.insertAdjacentHTML('beforeend', `<style>
:root { --text:#1f2937; --dim:#6b7280; --line:#e5e7eb; --paper:#fff; --soft:#f8fafc;
  --heat:#f97316; --cool:#3b82f6; --dhw:#f59e0b; --defrost:#06b6d4; --freeze:#0369a1; --danger:#dc2626; --warn:#d97706; --ok:#16a34a; --set:#16a34a;
  --ts-text:var(--text); --ts-dim:var(--dim); --ts-grid:var(--line); --ts-panel:var(--paper); --ts-card:#fff; --ts-danger:var(--danger); }
* { box-sizing:border-box }
body { margin:0; background:#e5e7eb; font:13px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif; color:var(--text) }
.page { width:1000px; max-width:100%; margin:20px auto; background:var(--paper); padding:36px 44px; box-shadow:0 1px 4px rgba(0,0,0,.12) }
h1 { font-size:22px; margin:0 } h2 { font-size:15px; margin:26px 0 10px; padding-bottom:6px; border-bottom:2px solid var(--text) }
h2 .sub { font-weight:400; font-size:12px }
.sub { color:var(--dim) } .note { color:var(--dim); font-size:11px; margin-top:4px }
.head { display:flex; justify-content:space-between; align-items:flex-start; gap:24px }
.head .meta { display:grid; grid-template-columns:auto auto; gap:2px 14px; font-size:12px }
.head .meta > span:nth-child(odd) { color:var(--dim) }
.cov { margin-top:14px; display:flex; flex-wrap:wrap; gap:4px 18px; font-size:12px; padding:8px 12px; background:var(--soft); border:1px solid var(--line); border-radius:6px }
.cov b { font-weight:600 }
.find { display:grid; grid-template-columns:22px 1fr; gap:4px 10px; padding:10px 12px; border:1px solid var(--line); border-left:4px solid var(--c); border-radius:6px; margin-bottom:8px; break-inside:avoid }
.find .ic { color:var(--c); font-weight:700; font-size:15px; line-height:1.2 }
.find .t { font-weight:600 } .find .ev, .find .chk { grid-column:2; color:var(--dim); font-size:12px } .find .chk { color:var(--text) }
.find .chk:before { content:"Check: "; font-weight:600 } .find a { color:inherit }
.tiles { display:grid; grid-template-columns:repeat(4,1fr); gap:10px }
.tile { border:1px solid var(--line); border-radius:6px; padding:10px 12px; break-inside:avoid }
.tile .l { color:var(--dim); font-size:11px; text-transform:uppercase; letter-spacing:.04em }
.tile .v { font-size:20px; font-weight:600; margin-top:2px } .tile .s { color:var(--dim); font-size:11.5px }
.legend { display:flex; gap:16px; flex-wrap:wrap; font-size:12px; color:var(--dim); margin:4px 0 6px }
.legend i { display:inline-block; width:12px; height:12px; border-radius:2px; vertical-align:-2px; margin-right:5px }
.legend i.ln { height:3px; vertical-align:3px; border-radius:0 }
.chart { height:300px; break-inside:avoid } .bars { height:280px; break-inside:avoid }
table { width:100%; border-collapse:collapse; font-size:12px; font-variant-numeric:tabular-nums }
thead { display:table-header-group } tr { break-inside:avoid }
th { text-align:left; font-weight:600; color:var(--dim); border-bottom:1px solid var(--text); padding:5px 6px }
td { border-bottom:1px solid var(--line); padding:4px 6px } td.n, th.n { text-align:right }
tr.flag td { background:#fff7ed } tr.gap td { color:var(--dim) }
.code { display:inline-block; min-width:30px; padding:0 4px; border:1px solid var(--line); border-radius:4px; background:var(--soft); font:600 10.5px/16px ui-monospace,Consolas,monospace; color:var(--dim); text-align:center }
td.cd { width:40px; padding-right:0 } .kv table td.cd { color:var(--dim) }
.tag { display:inline-block; padding:0 6px; border-radius:9px; font-size:11px; background:var(--soft); border:1px solid var(--line) }
.tag.bad { color:var(--danger); border-color:#fecaca; background:#fef2f2 } .tag.warn { color:var(--warn); border-color:#fde68a; background:#fffbeb }
.hist { display:flex; align-items:flex-end; gap:6px; height:110px; margin-top:22px; padding:0 4px; border-bottom:1px solid var(--text) }
.hist div { flex:1; background:var(--dhw); position:relative; border-radius:2px 2px 0 0 } .hist div.short { background:var(--warn) }
.hist div span { position:absolute; top:-16px; width:100%; text-align:center; font-size:11px; color:var(--dim) }
.histx { display:flex; gap:6px; padding:3px 4px 0; font-size:11px; color:var(--dim) } .histx span { flex:1; text-align:center }
.kv { display:grid; grid-template-columns:repeat(3,1fr); gap:0 22px } .kv table td:first-child { color:var(--dim) }
tr.chg td { background:#fffbeb } tr.chg td.v { font-weight:600 } td.det { color:var(--dim); font-size:11px } .det { color:var(--dim); font-size:11px }
tr.cat td { font-weight:600; background:var(--soft); border-bottom:1px solid var(--text); padding-top:8px }
.rng { display:inline-flex; border:1px solid var(--line); border-radius:6px; overflow:hidden; margin-left:auto }
.rng button { font:inherit; font-size:12px; border:0; border-left:1px solid var(--line); background:#fff; padding:3px 10px; cursor:pointer; color:var(--text) }
.rng button:first-child { border-left:0 } .rng button.on { background:var(--text); color:#fff } .rng button:disabled { color:var(--line); cursor:default }
.verdict { margin-top:14px; font-size:15px; font-weight:600; padding:10px 14px; border-radius:6px; background:#f0fdf4; border:1px solid #bbf7d0 }
.verdict.warn { background:#fffbeb; border-color:#fde68a } .verdict.bad { background:#fef2f2; border-color:#fecaca }
.units { display:inline-flex; border:1px solid var(--line); border-radius:5px; overflow:hidden; width:max-content }
.units button { font:inherit; font-size:11px; border:0; background:#fff; padding:1px 8px; cursor:pointer; color:var(--dim) } .units button.on { background:var(--text); color:#fff }
.two { display:grid; grid-template-columns:1fr 1fr; gap:28px }
.foot { margin-top:28px; color:var(--dim); font-size:11px; border-top:1px solid var(--line); padding-top:8px }
.empty { color:var(--dim); padding:8px 0 }
@media print {
  body { background:#fff } .page { box-shadow:none; margin:0; width:auto; padding:0 } h2 { break-after:avoid }
  .brk { break-before:page } .rng, .rnghint { display:none }
  .units { border:0 } .units button:not(.on) { display:none } .units button.on { background:none; color:inherit; padding:0 }
}
</style>`);
document.body.insertAdjacentHTML('beforeend', ['p1', 'p2', 'p3', 'p4', 'p5'].map((id, i) => `<div class="page${i > 2 ? ' brk' : ''}" id="${id}"></div>`).join(''));
const $ = id => document.getElementById(id);

// ---- units and formatting ----
const stored = (() => { try { return localStorage.getItem('reportUnit'); } catch { return null; } })();
const unitPick = /^#[CF]$/.test(location.hash) ? location.hash[1] : stored;
const F = (unitPick || (M.prefs.temp_unit === 'fahrenheit' ? 'F' : 'C')) === 'F';
const U = F ? '°F' : '°C';
const tc = c => F ? c * 1.8 + 32 : c;                      // absolute temperature
const tk = c => F ? c * 1.8 : c;                           // temperature difference
const t10 = v => v == null ? null : F ? v * 1.8 + 320 : v; // tenths, for charts
const T0 = c => c == null ? '--' : `${Math.round(tc(c))} ${U}`;
const T1 = c => c == null ? '--' : `${tc(c).toFixed(1)} ${U}`;
const n1 = c => c == null ? '--' : tc(c).toFixed(F ? 0 : 1).replace(/\.0$/, '');
const C = v => v == null ? '--' : T0(v / 10);              // whole-degree unit sensor, tenths
const C1 = v => v == null ? '--' : T1(v / 10);             // averaged, tenths
const dC = v => v == null ? '--' : `${tk(v / 10).toFixed(1)} ${U}`;
const fmtD = (t, o) => new Date(t * 1000).toLocaleString('en-US', { timeZone: TZ, ...o });
const h12 = !(M.prefs.format_24h ?? M.status?.time?.format_24h);
const day = t => fmtD(t, { weekday: 'short', month: 'short', day: 'numeric' });
const clock = t => fmtD(t, { hour: 'numeric', minute: '2-digit', hour12: h12 });
const when = t => `${day(t)} ${clock(t)}`;
const dur = s => s >= 3600 ? `${Math.floor(s / 3600)} h ${Math.round(s % 3600 / 60)} min` : s >= 60 ? `${Math.round(s / 60)} min` : `${Math.round(s)} s`;
const kwh = wh => `${(wh / 1000).toFixed(1)} kWh`;
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
const codePill = c => `<span class="code">${esc(c)}</span>`;
const plural = (n, one, many = one + 's') => `${n} ${n === 1 ? one : many}`;
const sum = (xs, f) => xs.reduce((a, x) => a + (f(x) || 0), 0);
const median = xs => { const s = xs.slice().sort((a, b) => a - b); return s.length ? s[s.length >> 1] : null; };
const min = xs => xs.length ? Math.min(...xs) : null, max = xs => xs.length ? Math.max(...xs) : null;
const len = r => r.end - r.start;
const flow = l => l == null ? '--' : F ? `${(l / 3.785).toFixed(1)} US gpm` : `${l} L/min`;
const flow2 = l => l == null ? '' : F ? `${l} L/min` : `${(l / 3.785).toFixed(1)} US gpm`;
const MODE = { hot_water: ['Tank heating', 'var(--dhw)'], heating: ['Space heating', 'var(--heat)'], cooling: ['Cooling', 'var(--cool)'] };
const MODE_KEYS = Object.keys(MODE);
const UNIT_MODE = { cooling: 'Cooling', heating: 'Heating', hot_water: 'Hot water', hot_water_cooling: 'Auto (hot water + cooling)',
  freeze_protection: 'Freeze protection', mode_3: 'Mode 3', mode_4: 'Mode 4' };
const modeName = m => UNIT_MODE[m] || String(m || 'unknown').replace(/_/g, ' ');

// ---- data ----
const days = DATA.days, full = days.filter(d => !d.missing);
const ser = s => s.tank.map((_, i) => ({ t: s.t0 + i * s.step, tank: s.tank[i], set: s.set[i], out: s.out[i], in: s.in[i] }));
const overview = ser(DATA.overview), detail = ser(DATA.detail);
const calls = DATA.cycles, heatCalls = calls.filter(c => c.mode !== 'cooling');
const present = MODE_KEYS.filter(m => calls.some(c => c.mode === m));
const callWord = present.length === 1 && present[0] === 'cooling' ? 'cooling call' : heatCalls.length === calls.length ? 'heating call' : 'call';
const callsWord = callWord + 's';
const E = DATA.events, ev = t => E.filter(e => e.type === t && e.timestamp >= M.from);
const inDay = (t, d) => t >= d.t0 && t < d.t1;
const dayOf = t => days.find(d => inDay(t, d));
const modeSum = (d, k) => d.missing ? 0 : MODE_KEYS.reduce((a, m) => a + (d[k]?.[m] || 0), 0);
const eIn = sum(full, d => d.electrical_wh.total), heat = sum(full, d => modeSum(d, 'thermal_wh'));
const covered = sum(full, d => modeSum(d, 'covered_electrical_wh'));
const runS = sum(full, d => modeSum(d, 'run_seconds')), starts = sum(full, d => d.starts);
const runBy = MODE_KEYS.map(m => [m, sum(full, d => d.run_seconds?.[m])]).filter(([, s]) => s > 0);
const nDays = days.length, nFull = full.length;
const lastDay = days[nDays - 1], todayPartial = M.to < lastDay.t1;
const short = calls.filter(r => len(r) < 600 && !r.clipStart && !r.open);
// Why a short call stopped, from an event logged during it or within 90 s of
// its last running sample (30 s samples, plus logging lag). Unexplained calls
// are thermostat stops.
const STOP = [['error_appeared', e => `ended by ${e.fault_code || 'a fault'}${e.fault_label ? ` ${e.fault_label}` : ''}`],
  ['power_off', () => 'unit switched off'], ['mode_changed', e => /freeze/.test(e.from_mode + e.to_mode) ? null : 'mode changed'],
  ['setpoint_changed', () => 'setpoint changed'], ['disconnected', () => 'unit stopped answering'],
  ['system_start', () => 'controller restarted']];
const stopCause = r => { for (const [t, f] of STOP) for (const e of E) if (e.type === t && e.timestamp >= r.start && e.timestamp <= r.end + 90) { const c = f(e); if (c) return c; } return null; };
short.forEach(r => { r.cause = stopCause(r); });
const medLen = calls.length ? Math.round(median(calls.map(len)) / 60) : null;

// Outdoor: daily max far above the day's average isn't weather. On a day with
// defrosts it is the coil heating the air sensor (the firmware now leaves
// defrost out of the day's figures); without one it's a suspect sensor.
const jumped = d => d.outdoor_c && d.outdoor_c.max != null && d.outdoor_c.max - d.outdoor_c.avg > 20;
const spikes = full.filter(d => jumped(d) && !d.defrost?.count);
const outDays = full.filter(d => d.outdoor_c && d.outdoor_c.min != null);
const outMin = min(outDays.map(d => d.outdoor_c.min)), outMax = max(outDays.filter(d => !jumped(d)).map(d => d.outdoor_c.max));
const callAir = r => r.air ?? dayOf(r.start)?.outdoor_c?.run_avg ?? null;
// Average while running, weighted by call length (calls with an outdoor reading only).
const airCalls = calls.filter(r => callAir(r) != null), airS = sum(airCalls, len);
const outRunAvg = airS ? sum(airCalls, r => callAir(r) * len(r)) / airS : null;

// Defrost, measured against heating-direction run time.
const DF = DATA.defrosts.map(d => ({ ...d, end: d.end ?? M.to }));
const heatS = sum(heatCalls, len), heatH = heatS / 3600;
const dfMax = max(DF.map(d => d.end - d.start)), dfTot = sum(DF, d => d.end - d.start);
const dfClose = DF.filter((d, i) => i && d.start - DF[i - 1].end < 1800).length;
const dfCall = d => heatCalls.find(c => d.start < c.end + 120 && d.end > c.start - 120);
const bandDefs = [[-99, -5, `below ${T0(-5)}`], [-5, 0, `${n1(-5)} to ${T0(0)}`], [0, 5, `${n1(0)} to ${T0(5)}`], [5, 99, `above ${T0(5)}`]];
const bands = bandDefs.map(([lo, hi, l]) => {
  const rr = heatCalls.filter(r => { const a = callAir(r); return a != null && a >= lo && a < hi; });
  return { l, h: sum(rr, len) / 3600, n: DF.filter(d => rr.includes(dfCall(d))).length };
}).filter(b => b.h > 0 || b.n > 0);
const dfNoAir = DF.filter(d => { const c = dfCall(d); return !c || callAir(c) == null; }).length;

// COP: measured heat over the input it covers, energy-weighted over calls of 10 min or more.
const copCalls = calls.filter(r => r.cop != null && r.ce > 0 && len(r) >= 600);
const copCe = sum(copCalls, r => r.ce), copAll = copCe ? sum(copCalls, r => r.q) / copCe : null;
const copFrom = min(copCalls.map(r => r.start)), copTo = max(copCalls.map(r => r.end));
const P = M.perf || {}, PS = P.status || {};
const sensorText = s => !s ? '' : s.source === 'bacnet_ip' ? `BACnet ${s.host || ''}` : s.source === 'modbus_tcp' ? `Modbus TCP ${s.host || ''}` : (s.source || '').replace(/_/g, ' ');
const external = (M.status?.readings?.perf_source || PS.source) === 'external';
const copSource = external ? `external supply/return sensors (${sensorText(P.sensors?.supply)})` : `the unit's own water sensors`;
const fluid = !P.fluid ? '--' : P.fluid === 'water' ? 'Water' : `${P.glycol_pct ?? '?'}% ${P.fluid.replace(/_/g, ' ')}`;

// Tank and setpoint.
const tankV = overview.map(s => s.tank).filter(v => v != null), setV = overview.map(s => s.set).filter(v => v != null);
const unitSet = max(setV);
const dhwCalls = calls.filter(r => r.mode === 'hot_water'), endTank = dhwCalls.map(r => r.tank1).filter(v => v != null);
const externalDemand = endTank.length >= 3 && unitSet != null && max(endTank) <= unitSet - 50;

// Faults: spans paired by the controller, details from the event log.
const FL = DATA.faults.slice().sort((a, b) => a.start - b.start);
const activeNow = M.status?.has_error ? M.status.error : null;
const fCodes = [...new Set(FL.map(f => f.fault_code || '?'))];
const near = (t, w) => E.filter(e => e.timestamp >= t - w && e.timestamp <= t + w);
const lastBefore = (t, type, f = () => true) => E.filter(e => e.type === type && e.timestamp <= t && f(e)).pop();
const firstAfter = (t, type) => E.find(e => e.type === type && e.timestamp > t);
function faultStory(f) {
  const parts = [], on = lastBefore(f.start, 'compressor_on'), off = lastBefore(f.start, 'compressor_off');
  if (on && (!off || off.timestamp < on.timestamp)) parts.push(`Compressor had been running ${dur(f.start - on.timestamp)}`);
  else if (off) parts.push(`Compressor was off (stopped ${dur(f.start - off.timestamp)} earlier)`);
  const fz = lastBefore(f.start, 'mode_changed', e => e.to_mode === 'freeze_protection');
  if (fz && f.start - fz.timestamp < 1800) parts.push(`freeze protection ran ${dur(f.start - fz.timestamp)} before (water near ${T0(2)})`);
  let s = parts.join('; ') + '.';
  if (f.end) {
    s += ` Cleared after ${dur(f.end - f.start)}`;
    const next = firstAfter(f.end, 'compressor_on'), nextOff = next && firstAfter(next.timestamp, 'compressor_off');
    if (nextOff && nextOff.timestamp - next.timestamp >= 300) s += `; the next start at ${clock(next.timestamp)} ran ${dur(nextOff.timestamp - next.timestamp)}`;
    s += '.';
  } else s += ' Still active when the report was made.';
  return s;
}

// Controller restarts.
const restarts = ev('system_start'), fw = ev('firmware_updated'), wd = ev('watchdog_reset'), crashes = ev('application_crash'), brown = ev('brownout_reset');
const unexplained = Math.max(0, restarts.length - fw.length - wd.length - crashes.length - brown.length);
const sensorLost = ev('ext_sensor_lost');

// Freeze protection.
const FZ = DATA.freeze.map(z => ({ ...z, end: z.end ?? M.to }));
const fzDays = [...new Set(FZ.map(z => dayOf(z.start)).filter(Boolean))];
const fzLows = fzDays.map(d => d.outdoor_c?.min).filter(v => v != null);

// Parameters.
const A = M.advanced?.params || {}, params = Object.values(A);
const changed = params.filter(p => p.default != null && p.value !== p.default).sort((a, b) => a.ap - b.ap);
const isTemp = p => p.temperature_kind ? p.temperature_kind !== 'none' : p.unit === '°C';
const pnum = (p, v) => !isTemp(p) ? `${v}` : `${Math.round(p.temperature_kind === 'differential' ? tk(v) : tc(v))}`;
const punit = p => isTemp(p) ? U : p.unit;
const pv = (p, v) => { const o = p.options && p.options.find(o => o.id === v); return v == null ? '--' : o ? o.label : `${pnum(p, v)}${punit(p) ? ' ' + punit(p) : ''}`; };

// ---- findings ----
const sevC = { bad: 'var(--danger)', warn: 'var(--warn)', info: 'var(--cool)', ok: 'var(--ok)' };
const sevI = { bad: '!', warn: '▲', info: 'i', ok: '✓' };
const finding = (sev, t, evid, chk) => `<div class="find" style="--c:${sevC[sev]}"><div class="ic">${sevI[sev]}</div><div class="t">${t}</div>${evid ? `<div class="ev">${evid}</div>` : ''}${chk ? `<div class="chk">${chk}</div>` : ''}</div>`;
const unit = [], ctrl = [], verdict = [];
const helpLinks = [...new Map(FL.filter(f => f.help_url).map(f => [f.fault_code, f])).values()]
  .map(f => `<a href="${esc(f.help_url)}">Arctic ${esc(f.fault_code)} article</a>`).join(' · ');
if (activeNow) {
  unit.push(finding('bad', `Fault active now: ${esc(activeNow.code)} ${esc(activeNow.description || '')}`, FL.length ? faultStory(FL[FL.length - 1]) : '', helpLinks));
  verdict.push(['bad', `Fault ${activeNow.code} is active.`]);
}
if (FL.length) {
  const last = FL[FL.length - 1], counts = fCodes.map(c => `${c} ×${FL.filter(f => (f.fault_code || '?') === c).length}`).join(', ');
  const sev = FL.length > 2 || fCodes.length > 1 ? 'warn' : 'info';
  if (!activeNow || FL.length > 1) unit.push(finding(sev, `${plural(FL.length, 'fault')}: ${esc(counts)}`, `Most recent: ${esc(last.fault_code || '')} ${esc(last.fault_label || '')} at ${when(last.start)}. ${faultStory(last)}`,
    `${FL.length === 1 ? 'Isolated trip. If it repeats, compare the conditions in the fault context below.' : 'Repeated faults: compare the fault contexts below for a common pattern.'}${helpLinks ? ' ' + helpLinks : ''}`));
  verdict.push(['warn', FL.length === 1 && FL[0].end ? `one self-clearing ${FL[0].fault_code || ''} trip` : `${plural(FL.length, 'fault')} (${counts})`]);
} else unit.push(finding('ok', 'No faults', `No fault codes in the ${nDays} days.`));
if (spikes.length) {
  const sm = spikes.map(d => d.outdoor_c.max), sa = spikes.map(d => d.outdoor_c.avg);
  unit.push(finding('warn', `Outdoor reading jumped to ${n1(min(sm))}–${T0(max(sm))} on ${plural(spikes.length, 'day')}`,
    `On those days the average was ${n1(min(sa))} to ${T0(max(sa))}${outMax != null ? `, and the other days never went above ${T0(outMax)}` : ''}. Brief spikes like this aren't real air temperature.`,
    'Outdoor air sensor placement (direct sun, a nearby heat source) or wiring. The unit uses this sensor for defrost control.'));
  verdict.push(['warn', 'a suspect outdoor sensor']);
}
if (calls.length) {
  const shortest = min(calls.filter(r => !r.clipStart && !r.open).map(len));
  const dfRestarts = sum(calls, r => r.starts - 1);
  const pre = ev('compressor_on').filter(e => M.telemetryFrom != null && e.timestamp < M.telemetryFrom).length;
  const brief = Math.max(0, starts - calls.length - dfRestarts - pre);
  const recon = starts ? ` The controller counted ${plural(starts, 'compressor start')}${dfRestarts ? `: ${dfRestarts} are the stop and restart around each defrost` : ''}${pre ? `, ${pre} came before telemetry begins` : ''}${brief ? `, and ${brief} ${brief > 1 ? 'were' : 'was'} too brief to show in ${M.interval} s samples` : ''}.` : '';
  const why = short.filter(r => r.cause), thermo = short.length - why.length;
  const whyText = why.length ? ` ${why.slice(0, 4).map(r => `${day(r.start)} ${clock(r.start)} (${dur(len(r))}): ${esc(r.cause)}`).join('; ')}${why.length > 4 ? `; ${why.length - 4} more` : ''}.` : '';
  const shortHint = !short.length ? '' : (why.some(r => r.cause.startsWith('ended by')) ? 'A run cut short by a fault: see Faults below. ' : '')
    + (thermo ? `${why.length ? `The other ${thermo === 1 ? 'one' : thermo} stopped on the thermostat: check t` : 'T'}ank differential / hysteresis, buffer-tank sensor position.` : '');
  unit.push(finding(short.length ? 'warn' : 'ok', short.length ? `${short.length} of ${calls.length} ${callsWord} were shorter than 10 minutes` : 'No short cycling',
    `${plural(calls.length, callWord, callsWord)}, median ${medLen} min${shortest != null ? `, shortest ${dur(shortest)}` : ''}.${whyText}${recon}`,
    shortHint.trim()));
  verdict.push(short.length ? ['warn', plural(short.length, `short ${callWord}`, `short ${callsWord}`)] : ['ok', 'no short cycling']);
} else unit.push(finding('info', 'The compressor did not run', `No compressor runs in the ${nDays} days.`));
if (externalDemand) unit.push(finding('info', 'Tank heating calls end well below the unit setpoint',
  `Calls end with the tank at ${C(min(endTank))} to ${C(max(endTank))} while the unit setpoint is ${C(unitSet)}, so an external control (thermostat or tank controller) is deciding when to heat. The unit setpoint is a ceiling here, not the target.`));
if (heatCalls.length) {
  const bad = dfClose || dfMax > 900;
  const bandTxt = bands.length ? ` By outdoor temperature (each call's average): ${bands.map(b => `${b.l} ${b.n} in ${b.h.toFixed(1)} h`).join(' · ')}${dfNoAir ? ` · outdoor unavailable ${dfNoAir}` : ''}.` : '';
  unit.push(finding(bad ? 'warn' : 'ok', !DF.length ? 'No defrosts' : bad ? 'Defrost needs a look' : 'Defrost looks normal',
    DF.length ? `${plural(DF.length, 'defrost')} in ${heatH.toFixed(1)} heating hours (${(DF.length / heatH).toFixed(1)} per hour), ${(100 * dfTot / heatS).toFixed(0)}% of heating time, longest ${dur(dfMax)}; ${dfClose ? `${dfClose} within 30 min of the previous one` : 'none within 30 min of another'}.${bandTxt}`
      : `None in ${heatH.toFixed(1)} heating hours.`));
}
if (copAll != null) unit.push(finding('info', `Estimated COP ${copAll.toFixed(1)}`,
  `Energy-weighted over ${plural(copCalls.length, 'call')} of 10 min or more, ${day(copFrom)} – ${day(copTo)} (per-call data is kept for a limited time). Heat is calculated from ${copSource} and the configured flow of ${flow(P.flow_lpm ?? M.status?.readings?.flow_lpm)}, so treat it as an estimate; start-up and defrost are included.`));
if (FZ.length) {
  unit.push(finding('info', `Freeze Protection ran ${plural(FZ.length, 'time')}`,
    `Water at the unit dropped to about ${T0(2)} while idle, so the unit circulated tank water for a short time. On ${plural(fzDays.length, 'day')}${fzLows.length ? ` with outdoor lows of ${n1(min(fzLows))} to ${T0(max(fzLows))}` : ''}; expected in cold weather.`));
}
if (sensorLost.length) ctrl.push(finding('warn', `COP sensors dropped out ${plural(sensorLost.length, 'time')}`, `External supply/return sensors stopped answering; COP falls back to the unit's sensors while they are out.`, 'Network path to the sensor gateway.'));
if (restarts.length) {
  const wdT = wd.slice(0, 3).map(e => when(e.timestamp)).join(', ') + (wd.length > 3 ? ', …' : '');
  const why = [fw.length ? `${fw.length} ${fw.length === 1 ? 'was a firmware update' : 'were firmware updates'}` : '',
    wd.length ? `${wd.length} ${wd.length === 1 ? 'was a watchdog reset' : 'were watchdog resets'} (${wdT})` : '',
    crashes.length ? `${plural(crashes.length, 'crash', 'crashes')}` : '', brown.length ? `${plural(brown.length, 'brownout')}` : ''].filter(Boolean);
  const sev = wd.length || crashes.length || brown.length || unexplained ? 'warn' : 'ok';
  ctrl.push(finding(sev, `Controller restarted ${plural(restarts.length, 'time')}`,
    `${why.length ? why.join(', ').replace(/^./, c => c.toUpperCase()) + '.' : ''}${unexplained ? ` ${unexplained === restarts.length ? 'None have' : `The other ${unexplained} ${unexplained === 1 ? 'has' : 'have'}`} no recorded cause (power loss or a manual restart).` : ''}`,
    sev === 'warn' ? 'Unexplained restarts point at power quality or controller firmware. Not a heat pump fault.' : ''));
} else ctrl.push(finding('ok', 'No controller restarts', ''));
const vSev = verdict.some(v => v[0] === 'bad') ? 'bad' : verdict.some(v => v[0] === 'warn') ? 'warn' : '';
const vFirst = activeNow ? verdict.shift()[1] : 'Running normally with no active fault.';
const vRest = verdict.map(v => v[1]).join(', ');
const covPct = M.coverage.expected ? (100 * M.coverage.connected / M.coverage.expected).toFixed(1) : '--';

// ---- page 1: summary ----
const year = fmtD(M.to, { year: 'numeric' });
const runSub = runBy.length === 1 ? `all ${MODE[runBy[0][0]][0].toLowerCase()}` : runBy.map(([m, s]) => `${(s / 3600).toFixed(1)} h ${MODE[m][0].toLowerCase()}`).join(' · ');
const dts = heatCalls.map(r => r.dt).filter(v => v != null);
const tile = (l, v, s) => `<div class="tile"><div class="l">${l}</div><div class="v">${v}</div><div class="s">${s}</div></div>`;
$('p1').innerHTML = `
<div class="head"><div><h1>Heat pump activity report</h1>
  <div class="sub" style="font-size:15px;margin-top:2px">${esc(M.prefs.device_name || M.info.hostname || 'Heat pump')}</div>
  <div class="sub" style="margin-top:6px">${day(M.from)} – ${day(M.to - 1)}, ${year} · ${nDays} days${todayPartial ? ' (today so far)' : ''} · times are controller local (${esc(TZ.replace(/_/g, ' '))})</div></div>
  <div class="meta"><span>Controller</span><span>${esc(M.info.hostname)}</span><span>Firmware</span><span>${esc(M.info.version)}</span>
  <span>Mode now</span><span>${esc(modeName(M.status?.mode))}${M.status?.unit_on === false ? ' (unit off)' : ''}</span><span>Generated</span><span>${fmtD(M.generated, { dateStyle: 'medium', timeStyle: 'short', hour12: h12 })}</span>
  <span>Units</span><span class="units"><button data-u="C" class="${F ? '' : 'on'}">°C</button><button data-u="F" class="${F ? 'on' : ''}">°F</button></span></div></div>
<div class="verdict ${vSev}">${esc(vFirst)}${vRest ? ' ' + esc(vRest.replace(/^./, c => c.toUpperCase())) + '.' : ''}</div>
<div class="cov"><span>Fluid <b>${esc(fluid)}</b></span><span>Flow <b>${flow(P.flow_lpm ?? M.status?.readings?.flow_lpm)}</b>${P.flow_lpm != null ? ` (${flow2(P.flow_lpm)}, configured)` : ''}</span>
  <span>COP sensors <b>${external ? esc(sensorText(P.sensors?.supply) || 'external') : 'unit'}</b></span>${dhwCalls.length ? `<span>Tank demand <b>${externalDemand ? 'external control' : 'unit setpoint'}</b>${externalDemand ? ' (inferred)' : ''}</span>` : ''}</div>
<div class="cov" style="margin-top:6px"><span>Telemetry <b>${covPct}%</b> of ${M.interval} s samples${M.telemetryFrom != null && M.telemetryFrom - M.from > 600 ? `, from ${when(M.telemetryFrom)}` : ''}, ${M.coverage.maxGap > 300 ? `longest gap ${dur(M.coverage.maxGap)}` : 'no gaps over 5 min'}</span>
  <span>Energy <b>${nFull}/${nDays} days</b></span><span>Event log <b>${M.eventsComplete ? 'complete' : `from ${when(M.eventsFrom)}`}</b></span>
  ${calls.length ? `<span>Per-call COP <b>${calls.filter(r => r.cop != null).length} of ${calls.length}</b> calls${M.runSummariesFrom ? ` (kept since ${day(M.runSummariesFrom)})` : ''}</span>` : ''}</div>
<h2>Heat pump findings</h2>
${unit.join('')}
<h2>Controller and data</h2>
${ctrl.join('')}
<h2>At a glance</h2>
<div class="tiles">
  ${tile('Compressor run time', `${(runS / 3600).toFixed(1)} h`, `${(runS / 3600 / Math.max(1, nFull)).toFixed(1)} h/day${runSub ? ' · ' + runSub : ''}`)}
  ${tile(callsWord.replace(/^./, c => c.toUpperCase()), calls.length, `${plural(starts, 'compressor start')}${medLen != null ? ` · median ${medLen} min` : ''}`)}
  ${tile('Energy used', `${(eIn / 1000).toFixed(0)} kWh`, `${(eIn / 1000 / Math.max(1, nFull)).toFixed(1)} kWh/day`)}
  ${tile('Heat measured', covered ? `${(heat / 1000).toFixed(0)} kWh` : '--', covered ? `for ${Math.round(100 * covered / Math.max(1, eIn))}% of energy used · COP ${(heat / covered).toFixed(1)} on that part` : 'no heat measurement')}
  ${tile('Tank (unit sensor)', tankV.length ? `${C(min(tankV)).replace(' ' + U, '')} to ${C(max(tankV))}` : '--', `${F ? 'whole-°C sensor' : '1 °C steps'}${unitSet != null ? ` · unit setpoint ${C(unitSet)}` : ''}`)}
  ${tile('Water ΔT while heating', dts.length ? dC(median(dts)) : '--', dts.length ? `median · range ${dC(min(dts))} to ${dC(max(dts))}` : 'no heating calls')}
  ${tile('Outdoor', outMin != null ? `${n1(outMin)} to ${T0(outMax)}` : '--', outRunAvg != null ? `avg while running ${T1(outRunAvg)}${airCalls.length < calls.length ? ` (${airCalls.length} of ${calls.length} calls)` : ''}${spikes.length ? ' · spikes excluded' : ''}` : 'no outdoor data while running')}
  ${tile('Defrosts', DF.length, DF.length && heatH ? `${(DF.length / heatH).toFixed(1)} per heating hour · longest ${dur(dfMax)}` : DF.length ? `longest ${dur(dfMax)}` : 'none')}
</div>
<h2>Settings that differ from factory default <span class="sub">— ${changed.length} of ${params.length}; full list at the end</span></h2>
${changed.length ? `<table>${changed.map(p => `<tr><td class="cd">${codePill(`AP${p.ap}`)}</td><td>${esc(p.name)}</td><td class="n"><b>${esc(pv(p, p.value))}</b></td><td class="n sub">default ${esc(pv(p, p.default))}</td></tr>`).join('')}</table>`
  : `<div class="empty">${params.length ? 'All parameters are at the factory default.' : 'Parameters could not be read.'}</div>`}`;

// ---- page 2: charts ----
const legend = items => `<div class="legend">${items.map(([l, bg, ln]) => `<span><i class="${ln ? 'ln' : ''}" style="background:${bg}"></i>${l}</span>`).join('')}</div>`;
const hatch = c => `repeating-linear-gradient(135deg,${c} 0 2px,transparent 2px 5px)`;
const callKeys = present.map(m => [`${MODE[m][0]} call`, `color-mix(in srgb,${MODE[m][1]} 35%,#fff);border:1px solid ${MODE[m][1]}`]);
$('p2').innerHTML = `
<h2 style="margin-top:0;display:flex;align-items:center;gap:12px">Activity <span class="sub" id="rngLbl"></span><span class="rng"><button data-step="-1" title="Earlier">‹</button>${[14, 7, 3, 1].filter(n => n <= nDays).map(n => `<button data-days="${n}">${n} d</button>`).join('')}<button data-step="1" title="Later">›</button></span></h2>
${legend([['Tank', 'var(--text)', 1], ['Unit setpoint', 'var(--set)', 1], ['Leaving water', '#e11d48', 1], ['Entering water', '#0ea5e9', 1], ...callKeys,
  ['Defrost', 'var(--defrost)'], ['Freeze Protection', hatch('var(--freeze)')], ['Fault', hatch('var(--danger)')]])}
<div class="chart" id="ov"></div><div class="note rnghint">Hover for values. Click a day in the charts below to zoom to it. Leaving/entering water drift toward outdoor temperature while the unit is idle (water standing in the unit).</div>
<div id="bdWrap"><h2 style="display:flex;gap:12px;align-items:baseline">Busiest day <span class="sub" id="bdLbl"></span></h2><div class="chart" id="bd" style="height:240px"></div></div>
<h2>Daily energy and efficiency</h2>
${legend([...MODE_KEYS.map(m => [MODE[m][0], MODE[m][1]]), ['COP (est.)', 'var(--set)', 1]])}
<div class="bars" id="en"></div>
<h2>Daily run time and starts</h2>
${legend([...MODE_KEYS.map(m => [`${MODE[m][0]} (h)`, MODE[m][1]]), ['Starts', 'var(--warn)', 1], ['Outdoor avg', '#64748b', 1], ['Outdoor min', 'var(--cool)', 1]])}
<div class="bars" id="rt" style="height:300px"></div>`;

// ---- page 3: by day, faults, events ----
const bins = [[0, 5], [5, 10], [10, 15], [15, 20], [20, 30], [30, 45], [45, 1e9]];
const counts = bins.map(([a, b]) => calls.filter(r => len(r) / 60 >= a && len(r) / 60 < b).length);
const mx = Math.max(1, ...counts);
const dayCop = d => { const c = modeSum(d, 'covered_electrical_wh'); return c >= 500 ? (modeSum(d, 'thermal_wh') / c).toFixed(1) : '--'; };
const dayRows = days.map(d => {
  const label = `${day(d.t0 + 3600)}${d === lastDay && todayPartial ? ' (so far)' : ''}`;
  if (d.missing) return `<tr class="gap"><td>${label}</td><td colspan="9">no energy data recorded</td></tr>`;
  const rr = calls.filter(r => inDay(r.start, d)), ls = rr.map(r => len(r) / 60), s = rr.filter(r => short.includes(r)).length;
  const partial = M.telemetryFrom != null && M.telemetryFrom > d.t0 + 600 && M.telemetryFrom < d.t1;
  const o = d.outdoor_c || {};
  return `<tr class="${s >= 3 ? 'flag' : ''}"><td>${label}${partial ? ' †' : ''}</td><td class="n">${d.starts}</td><td class="n">${rr.length}</td><td class="n">${(modeSum(d, 'run_seconds') / 3600).toFixed(1)} h</td>
    <td class="n">${ls.length ? `${Math.round(min(ls))}–${Math.round(max(ls))} min` : '--'}</td>
    <td class="n">${(d.electrical_wh.total / 1000).toFixed(1)}</td><td class="n">${(modeSum(d, 'thermal_wh') / 1000).toFixed(1)}</td><td class="n">${dayCop(d)}</td>
    <td class="n">${d.defrost?.count || ''}</td><td class="n">${n1(o.min)} / ${n1(o.avg)} / ${spikes.includes(d) ? `<span class="sub" title="sensor spike">${n1(o.max)}*</span>` : n1(o.max)}</td></tr>`;
}).join('');
const bootOf = e => E.some(x => x !== e && /firmware_updated|watchdog_reset|application_crash|brownout_reset/.test(x.type) && x.boot_id === e.boot_id);
const ROUTINE = /^(pump|fan|compressor|aux_heater)_|^connected$|^defrost_|^network_recovered$|^wifi_(restored|roamed)$|^web_server_recovered$|^ext_sensor_restored$/;
const notable = E.filter(e => e.timestamp >= M.from && !(e.type === 'system_start' && bootOf(e)) && !ROUTINE.test(e.type)
  && !(e.type === 'mode_changed' && /freeze/.test(e.from_mode + e.to_mode)));
const LBL = { system_start: ['Controller started', ''], firmware_updated: ['Firmware updated', ''], watchdog_reset: ['Watchdog reset', 'warn'],
  application_crash: ['Controller crash', 'bad'], brownout_reset: ['Brownout', 'warn'], error_appeared: ['Fault', 'bad'], error_cleared: ['Fault cleared', ''],
  network_unreachable: ['Network unreachable', 'warn'], mode_changed: ['Mode changed', ''], setpoint_changed: ['Setpoint changed', ''],
  param_changed: ['Parameter changed', 'warn'], power_on: ['Unit on', ''], power_off: ['Unit off', 'warn'], disconnected: ['Unit not answering', 'bad'],
  wifi_lost: ['WiFi lost', 'warn'], wifi_unstable: ['WiFi unstable', 'warn'], web_server_down: ['Web server down', 'warn'], https_fallback: ['HTTPS fallback', 'warn'],
  ext_sensor_lost: ['COP sensor lost', 'warn'] };
const paramByAp = Object.fromEntries(params.map(p => [`AP${p.ap}`, p]));
const evVal = (e, v, label) => label ?? (v == null ? '?' : e.temperature_kind === 'absolute' ? T0(v) : e.temperature_kind === 'differential' ? `${Math.round(tk(v))} ${U}` : `${v}${e.unit ? ' ' + e.unit : ''}`);
const evDetail = e => e.fault_code ? `${e.fault_code} ${e.fault_label || ''}` : e.to_version ? `${e.from_version || '?'} → ${e.to_version}`
  : e.type === 'param_changed' ? `${e.param} ${e.param_name || paramByAp[e.param]?.name || ''}: ${evVal(e, e.from, e.from_label)} → ${evVal(e, e.to, e.to_label)}`
  : e.type === 'setpoint_changed' ? `${(e.setpoint || '').replace(/_/g, ' ')} ${T0(e.from)} → ${T0(e.to)}`
  : e.watchdog ? `Restarted by watchdog (${e.watchdog})` : e.to_mode ? `${modeName(e.from_mode)} → ${modeName(e.to_mode)}` : '';
const evRows = notable.slice(-20).reverse().map(e => { const [l, c] = LBL[e.type] || [e.type.replace(/_/g, ' '), ''];
  return `<tr><td>${when(e.timestamp)}</td><td><span class="tag ${c}">${esc(l)}</span></td><td>${esc(evDetail(e))}</td></tr>`; }).join('');
const CTX = { compressor_on: 'Compressor on', compressor_off: 'Compressor off', pump_on: 'Pump on', pump_off: 'Pump off', fan_on: 'Fan on', fan_off: 'Fan off',
  error_appeared: 'FAULT', error_cleared: 'Fault cleared', defrost_start: 'Defrost start', defrost_end: 'Defrost end' };
const ctxRows = f => near(f.start, 600).map(e => {
  const l = e.type === 'mode_changed' ? `Mode → ${modeName(e.to_mode)}` : CTX[e.type] || e.type.replace(/_/g, ' ');
  const rel = e.timestamp - f.start, a = Math.abs(rel);
  const hit = e.type === 'error_appeared' && e.timestamp === f.start;
  return `<tr class="${hit ? 'flag' : ''}"><td>${clock(e.timestamp)}</td><td class="n sub">${rel < 0 ? '−' : rel > 0 ? '+' : ''}${Math.floor(a / 60)}:${String(a % 60).padStart(2, '0')}</td><td>${esc(l)}${e.fault_code ? ` ${esc(e.fault_code)}` : ''}</td></tr>`;
}).join('');
const ctxFaults = FL.slice(-3).reverse();
$('p3').innerHTML = `
<h2 style="margin-top:0">By day</h2>
<table><thead><tr><th>Day</th><th class="n">Starts</th><th class="n">${callsWord.replace(/^./, c => c.toUpperCase())}</th><th class="n">Run time</th><th class="n">Call length</th><th class="n">kWh in</th><th class="n">kWh heat</th><th class="n">COP</th><th class="n">Defrosts</th><th class="n">Outdoor min/avg/max ${U}</th></tr></thead><tbody>${dayRows}</tbody></table>
<div class="note">Starts are compressor starts counted by the controller; a call with a defrost has 3 (run, defrost, run).${M.telemetryFrom != null && M.telemetryFrom - M.from > 600 ? ` † telemetry begins ${clock(M.telemetryFrom)}, so earlier calls that day are not listed.` : ''}${spikes.length ? ' * outdoor sensor spike.' : ''}</div>
<div class="two" style="margin-top:6px"><div>
<h2>Call length</h2>${calls.length ? `<div class="hist">${counts.map((c, i) => `<div class="${i < 2 ? 'short' : ''}" style="height:${100 * c / mx}%"><span>${c}</span></div>`).join('')}</div>
<div class="histx">${bins.map(([a, b]) => `<span>${b > 1e8 ? `${a}+` : `${a}–${b}`}</span>`).join('')}</div><div class="note">minutes, defrost included · orange = shorter than 10 min</div>` : '<div class="empty">No compressor runs.</div>'}</div>
<div><h2>Faults</h2>${FL.length ? `<table><thead><tr><th>Started</th><th>Code</th><th class="n">Duration</th></tr></thead><tbody>
${FL.slice().reverse().map(f => `<tr><td>${when(f.start)}</td><td><span class="tag bad">${esc(f.fault_code || '?')}</span> ${esc(f.fault_label || '')}${f.help_url ? ` · <a href="${esc(f.help_url)}">Arctic article</a>` : ''}</td><td class="n">${f.end ? dur(f.end - f.start) : 'active'}</td></tr>`).join('')}</tbody></table>
${ctxFaults.map(f => `<h3 style="font-size:12px;margin:12px 0 4px">${esc(f.fault_code || 'Fault')} at ${when(f.start)} <span class="sub" style="font-weight:400">— 10 min either side, from the event log</span></h3>
<table>${ctxRows(f) || '<tr><td class="sub">No events logged around this fault.</td></tr>'}</table>`).join('')}` : '<div class="empty">No faults in the period.</div>'}</div></div>
<h2>Notable events <span class="sub">— latest ${Math.min(20, notable.length)} of ${notable.length}; restarts caused by an update or reset are folded into it; routine pump, fan, compressor, defrost and freeze-protection events are left out</span></h2>
${notable.length ? `<table><thead><tr><th style="width:170px">When</th><th style="width:160px">Event</th><th>Detail</th></tr></thead><tbody>${evRows}</tbody></table>` : '<div class="empty">None.</div>'}`;

// ---- page 4: every call ----
{
  const multi = present.length > 1;
  const rows = calls.slice().reverse().map(r => { const a = callAir(r);
    return `<tr class="${short.includes(r) ? 'flag' : ''}"><td>${day(r.start)}</td><td>${r.clipStart ? '…' : ''}${clock(r.start)} – ${r.open ? 'now' : clock(r.end)}${r.cause ? `<div class="sub">${esc(r.cause.replace(/^(ended by \S+).*/, '$1'))}</div>` : ''}</td>${multi ? `<td>${MODE[r.mode]?.[0] || esc(r.mode)}</td>` : ''}<td class="n">${Math.round(len(r) / 60)} min</td>
    <td class="n">${r.starts}</td><td class="n">${r.defrosts || ''}</td><td class="n">${a == null ? '--' : `${tc(a).toFixed(1)}${r.air == null ? '‡' : ''}`}</td>
    <td class="n">${C1(r.in)}</td><td class="n">${C1(r.out)}</td><td class="n">${dC(r.dt)}</td><td class="n">${C(r.tank0)} → ${C(r.tank1)}</td>
    <td class="n">${((r.e ?? r.wh) / 1000).toFixed(1)}</td><td class="n">${r.q != null ? (r.q / 1000).toFixed(1) : '--'}</td><td class="n">${r.cop != null ? r.cop.toFixed(1) : '--'}</td></tr>`; }).join('');
  $('p4').innerHTML = `
  <h2 style="margin-top:0">Every ${callWord} <span class="sub">— newest first; water temperatures averaged while running, skipping the first 5 min and defrosts</span></h2>
  ${calls.length ? `<table><thead><tr><th>Day</th><th>Time</th>${multi ? '<th>Mode</th>' : ''}<th class="n">Length</th><th class="n">Starts</th><th class="n">Defrosts</th><th class="n">Outdoor ${U}</th><th class="n">Entering</th><th class="n">Leaving</th><th class="n">ΔT</th><th class="n">Tank</th><th class="n">kWh in</th><th class="n">kWh heat</th><th class="n">COP</th></tr></thead><tbody>${rows}</tbody></table>
  <div class="note">‡ the day's average while running. Per-call outdoor and COP come from run summaries, which are kept for a limited time.</div>` : '<div class="empty">No compressor runs in the period.</div>'}`;
}

// ---- page 5: unit snapshot and parameters ----
{
  const S = M.status || {}, R = S.readings || {}, T = S.temperatures || {}, SP = S.setpoints || {};
  const onoff = v => v == null ? null : v ? 'On' : 'Off';
  // Rows are [name, value, unit, code]; a table with any code gets a code-pill
  // column (blank for code-less rows, so names line up).
  const kvt = (title, rows) => {
    const coded = rows.some(r => r[3]);
    return `<div><h2>${title}</h2><table>${rows.map(([k, v, u, c]) => `<tr>${coded ? `<td class="cd">${c ? codePill(c) : ''}</td>` : ''}<td>${esc(k)}</td><td class="n">${v == null || v === '' ? (v === '' ? '' : '--') : esc(v) + (u ? ` ${u}` : '')}</td></tr>`).join('')}</table></div>`;
  };
  // OEM codes, units and EEV travel come from the firmware's catalog.
  const CAT = M.catalog || {}, code = key => CAT[key]?.code || '';
  const eev = key => {
    const v = R[key], max = CAT[`readings.${key}`]?.max;
    return v == null ? null : max ? `${v} steps (${Math.min(100, Math.round(v / max * 100))} % open)` : `${v} steps`;
  };
  const idle = !S.compressor, run = v => idle ? null : v;
  const tempRows = [['Tank', T.tank, 'tank'], ['Outlet', T.outlet, 'outlet'], ['Inlet', T.inlet, 'inlet'], ['Outdoor air', T.outdoor, 'outdoor'],
    ['Outdoor coil', T.outdoor_coil, 'outdoor_coil'], ['Water HX', T.indoor_coil, 'indoor_coil'], ['Discharge', run(T.discharge), 'discharge'],
    ['Suction', run(T.suction), 'suction'], ['IPM module', run(T.ipm), 'ipm']].map(([k, v, key]) => [k, v == null ? null : Math.round(tc(v)), U, code(`temperatures.${key}`)]);
  const rd = (k, v, u, key) => [k, v, u, code(`readings.${key}`)];
  const readRows = [rd('Compressor', run(R.compressor_freq), 'Hz', 'compressor_freq'), rd('Fan', run(R.fan_rpm), 'RPM', 'fan_rpm'),
    rd('AC voltage', R.ac_voltage, 'V', 'ac_voltage'), rd('AC current', run(R.ac_current), 'A', 'ac_current'), rd('DC bus', R.dc_voltage, 'V', 'dc_voltage'),
    rd('Main EEV', eev('primary_eev'), '', 'primary_eev'), rd('Aux EEV', eev('aux_eev'), '', 'aux_eev'), rd('Input power', run(R.power_consumption), 'W', 'power_consumption'),
    ['Flow (configured)', R.flow_lpm != null ? `${flow(R.flow_lpm)} (${flow2(R.flow_lpm)})` : null], ['Fluid', fluid]];
  const ex = PS.sensors || {};
  const stateRows = [['Unit enabled', S.unit_on == null ? null : S.unit_on ? 'Yes' : 'No'], ['Mode', modeName(S.mode)], ['Operation', S.operation], ['Compressor', onoff(S.compressor)],
    ['Fan', onoff(S.fans)], ['Pump', onoff(S.pump)], ['Backup heater', onoff(S.aux_heater)], ['Defrosting', onoff(S.defrosting)]];
  const spRows = [['Hot water setpoint', T0(SP.hot_water)], ['Heating setpoint', T0(SP.heating)], ['Cooling setpoint', T0(SP.cooling)],
    ...(ex.supply?.configured ? [['External supply', T1(ex.supply.celsius)]] : []), ...(ex.return?.configured ? [['External return', T1(ex.return.celsius)]] : []),
    ['COP source', external ? `external (${sensorText(P.sensors?.supply)})` : 'unit sensors']];
  const fmtDetail = s => esc(s).replace(/\{T:(-?\d+)\}/g, (_, c) => T0(+c));
  const cats = [...new Set(params.map(p => p.category || 'Other'))];
  const pchg = ev('param_changed');
  const range = p => p.options ? '' : p.min != null && p.max != null ? `${pnum(p, p.min)} – ${pnum(p, p.max)}` : '';
  const rows = cats.map(c => `<tr class="cat"><td colspan="6">${esc(c)}</td></tr>` + params.filter(p => (p.category || 'Other') === c).sort((a, b) => a.ap - b.ap).map(p => {
    const chg = p.default != null && p.value !== p.default;
    return `<tr class="${chg ? 'chg' : ''}"><td>${codePill(`AP${p.ap}`)}</td><td>${esc(p.name)}${p.detail ? `<div class="det">${fmtDetail(p.detail)}</div>` : ''}</td>
      <td class="n v">${esc(pv(p, p.value))}</td><td class="n sub">${p.default != null ? esc(pv(p, p.default)) : ''}</td><td class="n sub">${esc(range(p))}</td>
      <td>${chg ? '<span class="tag warn">changed</span>' : ''}${p.read_only ? ' <span class="tag">read-only</span>' : ''}</td></tr>`;
  }).join('')).join('');
  $('p5').innerHTML = `
  <h2 style="margin-top:0">Unit snapshot <span class="sub">— read at ${when(M.generated)}; ${idle ? 'the unit was idle, so compressor readings show --' : 'the unit was running'}</span></h2>
  <div class="kv">${kvt('State', stateRows)}${kvt('Temperatures', tempRows)}${kvt('Readings', readRows)}</div>
  <div class="kv">${kvt('Setpoints and COP sensors', spRows)}${kvt('Active faults', (M.activeFaults || []).length ? M.activeFaults.map(f => [f.name || f.description || '', '', '', f.code]) : S.has_error && S.error ? [[S.error, '']] : [['None', '']])}
    ${kvt('Parameter changes in period', pchg.length ? pchg.slice(-8).map(e => [`${e.param} ${when(e.timestamp)}`, `${evVal(e, e.from, e.from_label)} → ${evVal(e, e.to, e.to_label)}`]) : [['None logged', '']])}</div>
  ${params.length ? `<h2 class="brk">Technician parameters <span class="sub">— ${params.length} AP parameters; ${changed.length} differ from the factory default (highlighted)</span></h2>
  <table><thead><tr><th style="width:52px">Code</th><th>Parameter</th><th class="n" style="width:80px">Value</th><th class="n" style="width:72px">Default</th><th class="n" style="width:96px">Range</th><th style="width:120px"></th></tr></thead><tbody>${rows}</tbody></table>` : ''}
  <div class="foot">Arctic Controller ${esc(M.info.version)} · ${esc(M.info.hostname)} · values read from the unit when the report was made.</div>`;
}

// ---- charts ----
const fmtT = (v, ctx) => ctx === 'axis' ? `${Math.round(v / 10)}°` : `${Math.round(v / 10)} ${U}`;
const span = x => `${clock(x.start)} – ${clock(x.end)} · ${dur(Math.max(60, x.end - x.start))}`;
const maxEnd = M.to, minStart = M.from;
function activityChart(el, getDays) {
  const ch = new TsChart(el, { id: el.id, header: { chips: false, key: false, tagsButton: false }, input: { pan: false }, axis: 'outside',
    timeFormat: (t, step) => step >= 86400 ? fmtD(t, { month: 'short', day: 'numeric' }) : clock(t) });
  ch.setEndTags(false);
  ch.addSource('s');
  const pT = ch.addPane({ id: 't', weight: 3, color: 'var(--dim)', maxTicks: 4, fmt: fmtT });
  ch.addLayer(pT, { id: 'set', name: 'Unit setpoint', geom: 'line', width: 1.5, color: 'var(--set)', value: s => t10(s.set) });
  ch.addLayer(pT, { id: 'sup', name: 'Leaving water', geom: 'line', width: 1, color: '#e11d48', value: s => t10(s.out) });
  ch.addLayer(pT, { id: 'ret', name: 'Entering water', geom: 'line', width: 1, color: '#0ea5e9', value: s => t10(s.in) });
  ch.addLayer(pT, { id: 'tank', name: 'Tank', geom: 'line', width: 2, color: 'var(--text)', value: s => t10(s.tank) });
  ch.setCard((c, card) => {
    const tol = getDays() * 86400 / 500, hit = xs => xs.find(x => c.time >= x.start - tol && c.time <= x.end + tol);
    card.add('title', when(c.time));
    card.addLayers();
    const r = hit(calls), z = hit(FZ), f = hit(FL.map(x => ({ ...x, end: x.end ?? M.to })));
    const d = DF.find(x => c.time >= x.start - tol / 4 && c.time <= x.end + tol / 4);
    if (r) {
      card.add('block', `${MODE[r.mode]?.[0] || r.mode} call`, span(r), MODE[r.mode]?.[1] || 'var(--dhw)');
      card.add('text', [`Tank ${C(r.tank0)} → ${C(r.tank1)}`, r.dt != null ? `ΔT ${dC(r.dt)}` : '', r.cop ? `COP ${r.cop.toFixed(1)}` : '',
        r.defrosts ? plural(r.defrosts, 'defrost') : ''].filter(Boolean).join(' · '));
    }
    if (d) card.add('block', 'Defrost', span(d), 'var(--defrost)');
    if (z) card.add('block', 'Freeze Protection', span(z), 'var(--freeze)');
    if (f) card.add('block', `Fault ${f.fault_code || ''}`, span(f), 'var(--danger)');
  });
  present.forEach(m => ch.addKey(m, MODE[m][0], MODE[m][1], { fillStrength: '35%' }));
  ch.addKey('defrost', 'Defrost', 'var(--defrost)', { fillStrength: '100%' });
  ch.addKey('freeze', 'Freeze Protection', 'var(--freeze)', 'hatch');
  ch.addKey('fault', 'Fault', 'var(--danger)', 'hatch');
  ch.show = (start, end) => {
    const fine = end - start <= 3 * 86400, all = fine ? detail : overview;
    const samples = all.filter(s => s.t >= start - 600 && s.t <= end + 600);
    ch.setDataset({ requestId: 0, start, end, sources: { s: { samples, time: 't', gap: fine ? 300 : 1200 } },
      applied: () => {
        ch.setSpans(0, calls.map(r => ({ start: r.start, end: r.end, minWidth: 2, key: r.mode })));
        ch.setSpans(1, DF.map(d => ({ start: d.start, end: d.end, minWidth: 2, key: 'defrost' })));
        ch.setSpans(2, FZ.map(x => ({ start: x.start, end: x.end, minWidth: 2, key: 'freeze' })));
        ch.setSpans(3, FL.map(f => ({ start: f.start, end: f.end ?? M.to, minWidth: 3, key: 'fault' })));
        ch.setNotes(FL.map(f => ({ time: f.start, text: f.fault_code || '?', color: 'var(--danger)' })), '⚠ ');
        ch.setStatus('ok');
      } });
    ch.flush();
  };
  return ch;
}
const rng = { days: Math.min(14, nDays), end: maxEnd };
const ov = activityChart($('ov'), () => rng.days);
function showRange() {
  // Today is partial, so the newest full-length range would start before the report does.
  const start = Math.max(minStart, rng.end - rng.days * 86400);
  ov.show(start, rng.end);
  $('rngLbl').textContent = rng.days === 1 ? day(start + 1) : `${day(start + 1)} – ${day(rng.end - 1)}`;
  document.querySelectorAll('.rng [data-days]').forEach(b => b.classList.toggle('on', +b.dataset.days === rng.days));
  document.querySelector('.rng [data-step="-1"]').disabled = start <= minStart;
  document.querySelector('.rng [data-step="1"]').disabled = rng.end >= maxEnd;
}
const clampEnd = () => { rng.end = Math.min(maxEnd, Math.max(minStart + rng.days * 86400, rng.end)); };
document.querySelector('.rng').addEventListener('click', e => {
  const b = e.target.closest('button'); if (!b) return;
  if (b.dataset.days) { rng.days = +b.dataset.days; if (rng.days >= nDays) rng.end = maxEnd; }
  else rng.end += +b.dataset.step * rng.days * 86400;
  clampEnd(); showRange();
});
// Clicking a day bar zooms the activity chart to that controller-local day.
document.addEventListener('tsbars:select', e => {
  const d = days.find(x => x.date === e.detail.key); if (!d) return;
  rng.days = 1; rng.end = Math.min(d.t1, maxEnd);
  if (rng.end - 86400 < d.t0) rng.end = d.t0 + 86400;
  clampEnd(); showRange();
  $('ov').scrollIntoView({ behavior: 'smooth', block: 'center' });
});
document.querySelector('.units').addEventListener('click', e => {
  const u = e.target.dataset.u; if (!u || (u === 'F') === F) return;
  try { localStorage.setItem('reportUnit', u); } catch { /* file:// storage can be off */ }
  location.hash = u; location.reload();
});
// The printout always shows the whole period.
window.addEventListener('beforeprint', () => { rng.days = Math.min(14, nDays); rng.end = maxEnd; showRange(); });
showRange();

// Busiest day: most calls, then most defrosts.
{
  const score = d => { const rr = calls.filter(r => inDay(r.start, d)); return rr.length * 10 + sum(rr, r => r.defrosts); };
  const bd = days.reduce((a, d) => score(d) > score(a) ? d : a, days[0]);
  if (!calls.length || !score(bd)) $('bdWrap').remove();
  else {
    const rr = calls.filter(r => inDay(r.start, bd)), o = bd.outdoor_c;
    $('bdLbl').textContent = `${day(bd.t0 + 3600)} — ${plural(rr.length, callWord, callsWord)}, ${plural(sum(rr, r => r.defrosts), 'defrost')}${o?.min != null ? `, outdoor ${n1(o.min)} / ${T0(o.avg)} min/avg` : ''}`;
    activityChart($('bd'), () => 1).show(bd.t0, bd.t1);
  }
}

const dl = k => new Date(`${k}T12:00:00Z`).toLocaleDateString('en-US', { timeZone: 'UTC', month: 'short', day: 'numeric' });
const dayTitle = b => new Date(`${b.key}T12:00:00Z`).toLocaleDateString('en-US', { timeZone: 'UTC', weekday: 'short', month: 'short', day: 'numeric' });
const mk = id => new TsBars($(id), { id, count: nDays, barWidth: 0.62, labels: { mode: 'every', text: b => dl(b.key) } });
const buckets = days.map(d => ({ key: d.date, data: d }));
const val = (d, f) => d.missing ? null : f(d);

const en = mk('en');
const pk = en.addPane({ id: 'kwh', weight: 230, unit: 'kWh', range: { auto: true, includeZero: true, minSpan: 500, minStep: 500 }, fmt: v => `${+(v / 1000).toFixed(1)}` });
MODE_KEYS.forEach(m => en.addSeries(pk, { id: m, name: MODE[m][0], color: MODE[m][1], stack: 'm', opacity: 1, value: b => val(b.data, d => d.electrical_wh[m] > 0 ? d.electrical_wh[m] : null) }));
const pc = en.addPane({ id: 'cop', weight: 80, minHeight: 60, unit: 'COP', range: { auto: true, minSpan: 10, minStep: 10 }, maxTicks: 2, fmt: v => `${(v / 10).toFixed(0)}` });
en.addSeries(pc, { id: 'cop', name: 'COP', color: 'var(--set)', geom: 'line', width: 1.5,
  value: b => val(b.data, d => modeSum(d, 'covered_electrical_wh') >= 500 ? Math.round(10 * modeSum(d, 'thermal_wh') / modeSum(d, 'covered_electrical_wh')) : null) });
en.setCard((b, card) => {
  const d = b.data;
  card.add('title', dayTitle(b));
  if (d.missing) { card.add('text', 'No energy data recorded'); return; }
  MODE_KEYS.forEach(m => { if (d.electrical_wh[m] > 0) card.add('value', MODE[m][0], `${kwh(d.electrical_wh[m])} in · ${kwh(d.thermal_wh?.[m] || 0)} out`, MODE[m][1]); });
  card.add('value', 'COP (est.)', dayCop(d), 'var(--set)');
  if (!d.electrical_wh.total) card.add('text', 'No runs');
  card.add('text', 'Click to zoom the activity chart');
});
en.setBuckets(buckets, { lastKey: lastDay.date }); en.setStatus('ok');

const rt = mk('rt');
const ph = rt.addPane({ id: 'h', weight: 200, unit: 'h', range: { auto: true, includeZero: true, minSpan: 10, minStep: 10 }, fmt: v => `${v / 10}` });
MODE_KEYS.forEach(m => rt.addSeries(ph, { id: 'h_' + m, name: MODE[m][0], color: MODE[m][1], stack: 'h', opacity: 1, value: b => val(b.data, d => d.run_seconds?.[m] > 0 ? Math.round(d.run_seconds[m] / 360) : null) }));
const ps = rt.addPane({ id: 's', weight: 80, minHeight: 60, unit: 'starts', range: { auto: true, includeZero: true, minSpan: 4, minStep: 2 }, maxTicks: 2, fmt: v => `${v}` });
rt.addSeries(ps, { id: 's', name: 'Starts', color: 'var(--warn)', geom: 'line', width: 1.5, value: b => val(b.data, d => d.starts) });
const po = rt.addPane({ id: 'o', weight: 90, minHeight: 60, unit: U, range: { auto: true, includeZero: true, minSpan: 50, minStep: 50 }, maxTicks: 2, fmt: v => `${v / 10}` });
rt.addSeries(po, { id: 'oa', name: 'Outdoor avg', color: '#64748b', geom: 'line', width: 1.5, value: b => val(b.data, d => d.outdoor_c?.avg == null ? null : Math.round(tc(d.outdoor_c.avg) * 10)) });
rt.addSeries(po, { id: 'om', name: 'Outdoor min', color: 'var(--cool)', geom: 'line', width: 1.5, value: b => val(b.data, d => d.outdoor_c?.min == null ? null : Math.round(tc(d.outdoor_c.min) * 10)) });
rt.setCard((b, card) => {
  const d = b.data;
  card.add('title', dayTitle(b));
  if (d.missing) { card.add('text', 'No data recorded'); return; }
  const s = modeSum(d, 'run_seconds');
  card.add('value', 'Run time', s ? dur(s) : 'none', 'var(--dhw)');
  card.add('value', 'Starts', String(d.starts), 'var(--warn)');
  if (d.defrost?.count) card.add('value', 'Defrosts', `${d.defrost.count} · ${dur(d.defrost.seconds)}`, 'var(--defrost)');
  if (d.outdoor_c?.min != null) card.add('value', 'Outdoor', `${n1(d.outdoor_c.min)} / ${T0(d.outdoor_c.avg)} min/avg`, '#64748b');
  card.add('text', 'Click to zoom the activity chart');
});
rt.setBuckets(buckets, { lastKey: lastDay.date }); rt.setStatus('ok');
})();
