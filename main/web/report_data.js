    // ---- Activity report: fetch the last 14 days and save a standalone HTML file ----
    // The renderer (report.js) and the chart library are inlined as text, so
    // the saved file opens anywhere, offline, with no link back to the controller.
    const REPORT_DAYS = 14;
    const REPORT_WINDOW = 28800;
    const REPORT_MODE = { 1: "heating", 2: "cooling", 3: "hot_water" };
    // The time presets offered in Settings > Time, as IANA zones for Intl.
    const POSIX_IANA = {
      "EST5EDT,M3.2.0,M11.1.0": "America/New_York", "CST6CDT,M3.2.0,M11.1.0": "America/Chicago",
      "MST7MDT,M3.2.0,M11.1.0": "America/Denver", "PST8PDT,M3.2.0,M11.1.0": "America/Los_Angeles",
      "AST4ADT,M3.2.0,M11.1.0": "America/Halifax", "GMT0BST,M3.5.0/1,M10.5.0": "Europe/London",
      "CET-1CEST,M3.5.0,M10.5.0/3": "Europe/Paris", "JST-9": "Asia/Tokyo", "CST-8": "Asia/Shanghai",
      "IST-5:30": "Asia/Kolkata", "AEST-10AEDT,M10.1.0,M4.1.0/3": "Australia/Sydney", "UTC0": "UTC"
    };
    const zoneOk = tz => { try { new Intl.DateTimeFormat("en", { timeZone: tz }); return !!tz; } catch { return false; } };
    function reportZone(timeConfig, location) {
      const iana = location && location.iana_tz;
      if (location && location.tz_auto && zoneOk(iana)) return iana;
      const preset = timeConfig && POSIX_IANA[timeConfig.timezone];
      if (preset) return preset;
      if (zoneOk(iana)) return iana;
      return Intl.DateTimeFormat().resolvedOptions().timeZone || "UTC";
    }
    // Minutes east of UTC for a zone at an instant.
    function zoneOffset(tz, ms) {
      const p = Object.fromEntries(new Intl.DateTimeFormat("en-US", { timeZone: tz, hourCycle: "h23",
        year: "numeric", month: "numeric", day: "numeric", hour: "numeric", minute: "numeric", second: "numeric" })
        .formatToParts(new Date(ms)).map(x => [x.type, +x.value]));
      return Math.round((Date.UTC(p.year, p.month - 1, p.day, p.hour, p.minute, p.second) - ms) / 60000);
    }
    // Unix seconds of local midnight starting `date` in `tz`.
    function zoneMidnight(date, tz) {
      const utc = Date.parse(`${date}T00:00:00Z`);
      let t = utc - zoneOffset(tz, utc) * 60000;
      t = utc - zoneOffset(tz, t) * 60000;
      return t / 1000;
    }
    const reportAvg = xs => xs.length ? Math.round(xs.reduce((a, b) => a + b, 0) / xs.length) : null;

    async function buildReportData(progress) {
      const step = text => progress && progress(text);
      step("Reading settings");
      const opt = p => p.catch(() => null);
      const [info, prefs, status, advanced, perf, timeConfig, location, latest, catalog, errors] = await Promise.all([
        api("/api/info"), api("/api/preferences"), api("/api/heatpump/status"), opt(api("/api/heatpump/advanced")),
        opt(api("/api/performance/config")), opt(api("/api/time/config")), opt(api("/api/location")),
        api(`/api/heatpump/temperature-history?window=${REPORT_WINDOW}`), opt(api("/api/heatpump/catalog")), opt(api("/api/heatpump/errors"))]);
      const tz = reportZone(timeConfig, location);
      const interval = latest.sample_interval_sec || 30;

      // Calendar: the last 14 controller-local days, today included.
      const daily = await api(`/api/energy/daily?from=${dayAdd(localToday(), -REPORT_DAYS)}`);
      const today = daily.to || localToday();
      const byDate = new Map((daily.days || []).map(d => [d.date, d]));
      const dates = Array.from({ length: REPORT_DAYS }, (_, i) => dayAdd(today, i + 1 - REPORT_DAYS));
      const start = date => byDate.has(date) ? energyMidnight(byDate.get(date)) : zoneMidnight(date, tz);
      const generated = Math.floor(Date.now() / 1000);
      const days = dates.map((date, i) => {
        const d = byDate.get(date);
        const t0 = start(date), t1 = i + 1 < dates.length ? start(dates[i + 1]) : zoneMidnight(dayAdd(date, 1), tz);
        return d ? { ...d, t0, t1 } : { date, t0, t1, missing: true };
      });
      const from = days[0].t0, to = Math.min(generated, days[days.length - 1].t1);

      // Telemetry in 8 h windows, newest first, plus the defrost, fault and
      // freeze-protection spans each window carries.
      const ends = [];
      for (let e = latest.end; e > from; e -= REPORT_WINDOW) ends.push(e);
      let done = 0;
      step(`Reading telemetry 0/${ends.length}`);
      const pages = await Promise.all(ends.map((e, i) => (i ? api(`/api/heatpump/temperature-history?window=${REPORT_WINDOW}&end=${e}`)
        : Promise.resolve(latest)).then(p => { step(`Reading telemetry ${++done}/${ends.length}`); return p; })));
      const byT = new Map(), spans = { defrosts: new Map(), faults: new Map(), freeze: new Map() };
      for (const pg of pages) {
        for (const s of pg.samples || []) if (s.t >= from && s.t < to) byT.set(s.t, s);
        for (const d of pg.defrosts || []) spans.defrosts.set(d.start, d);
        for (const f of pg.faults || []) spans.faults.set(`${f.start}:${f.fault_code}`, f);
        for (const z of pg.freeze_protection || []) spans.freeze.set(z.start, z);
      }
      const samples = [...byT.values()].sort((a, b) => a.t - b.t);
      const inWindow = x => x.start < to && (x.end ?? to) > from;
      const sorted = m => [...m.values()].filter(inWindow).sort((a, b) => a.start - b.start);
      const defrosts = sorted(spans.defrosts), faults = sorted(spans.faults), freeze = sorted(spans.freeze);

      step("Reading runs and events");
      const [summaries, events] = await Promise.all([fetchRunSummaries(from, to).catch(() => []), reportEvents(from)]);

      // Compressor runs from the samples, as buildRuns does, keeping what a
      // technician wants per run: tank at start and end, water temperatures.
      const runs = [];
      let cur = null;
      const close = () => {
        if (!cur) return;
        const end = cur.last + interval;
        const inDf = defrosts.reduce((a, d) => a + Math.max(0, Math.min(end, d.end ?? end) - Math.max(cur.start, d.start)), 0);
        const code = Object.entries(cur.modes).sort((a, b) => b[1] - a[1])[0]?.[0];
        runs.push({ start: cur.start, end, wh: cur.wh, mode: inDf * 2 > end - cur.start ? "defrost" : REPORT_MODE[code] || "heating",
          tank0: cur.tank0, tank1: cur.tank1 });
        cur = null;
      };
      for (const s of samples) {
        if (!s.run) { close(); continue; }
        if (cur && s.t - cur.last > interval * 2.5) close();
        if (!cur) cur = { start: s.t, last: s.t, wh: 0, modes: {}, tank0: s.tank, tank1: s.tank };
        cur.last = s.t;
        if (s.p != null) cur.wh += s.p * interval / 3600;
        if (s.tank != null) { cur.tank1 = s.tank; if (cur.tank0 == null) cur.tank0 = s.tank; }
        cur.modes[s.mode] = (cur.modes[s.mode] || 0) + 1;
      }
      const lastRun = cur;
      close();
      if (lastRun && runs.length) runs[runs.length - 1].open = lastRun.last + interval * 2.5 >= to;

      // Calls: compressor runs split only by a defrost (heat, defrost, heat) are one call.
      const cycles = [];
      for (const r of runs) {
        const last = cycles[cycles.length - 1], prev = last && last.segs[last.segs.length - 1];
        if (prev && r.start - prev.end <= 180 && (r.mode === "defrost" || prev.mode === "defrost")) { last.segs.push(r); last.end = r.end; }
        else cycles.push({ start: r.start, end: r.end, segs: [r] });
      }
      // A run the samples saw as "heating" is booked to whichever heating
      // profile the energy store used most when there is no run summary.
      const sumE = m => days.reduce((a, d) => a + (d.electrical_wh?.[m] || 0), 0);
      const heatProfile = sumE("hot_water") > sumE("heating") ? "hot_water" : "heating";
      const first = samples[0];
      for (const c of cycles) {
        const work = c.segs.filter(s => s.mode !== "defrost");
        const sm = summaries.filter(x => x.start < c.end + interval && x.end > c.start - interval);
        const top = sm.slice().sort((a, b) => b.electrical_wh - a.electrical_wh)[0];
        const sampled = work.length ? work[0].mode : "heating";
        c.mode = top?.mode || (sampled === "heating" ? heatProfile : sampled);
        c.starts = c.segs.length;
        const dfs = defrosts.filter(d => d.start < c.end + 120 && (d.end ?? d.start) > c.start - 120);
        c.defrosts = dfs.length;
        c.wh = Math.round(c.segs.reduce((a, s) => a + s.wh, 0));
        if (sm.length) {
          c.e = sm.reduce((a, x) => a + x.electrical_wh, 0); c.q = sm.reduce((a, x) => a + x.thermal_wh, 0);
          c.ce = sm.reduce((a, x) => a + x.covered_electrical_wh, 0);
          c.cop = c.ce >= RUN_COP_MIN_WH ? +(c.q / c.ce).toFixed(2) : null;
          const os = sm.reduce((a, x) => a + (x.outdoor_c != null ? x.outdoor_seconds || 0 : 0), 0);
          c.air = os ? +(sm.reduce((a, x) => a + (x.outdoor_c != null ? x.outdoor_c * (x.outdoor_seconds || 0) : 0), 0) / os).toFixed(1) : null;
        }
        // Water temperatures while working: skip each segment's first 5 min and defrost recovery.
        const ss = samples.filter(s => s.t >= c.start && s.t < c.end && work.some(h => s.t >= h.start + 300 && s.t < h.end)
          && !dfs.some(d => s.t >= d.start && s.t < (d.end ?? d.start) + 120));
        c.in = reportAvg(ss.map(s => s.in).filter(v => v != null));
        c.out = reportAvg(ss.map(s => s.out).filter(v => v != null));
        c.dt = c.in != null && c.out != null ? c.out - c.in : null;
        // Older firmware kept no outdoor average per run: use the median of
        // the call's samples, which also ignores brief sensor spikes.
        if (c.air == null) {
          const air = samples.filter(s => s.run && s.t >= c.start && s.t < c.end && s.air != null).map(s => s.air).sort((a, b) => a - b);
          if (air.length) c.air = air[air.length >> 1] / 10;
        }
        c.tank0 = c.segs[0].tank0; c.tank1 = c.segs[c.segs.length - 1].tank1;
        c.clipStart = !!first && c.start - first.t < interval * 2.5 && first.t - from < interval * 2.5;
        c.open = !!c.segs[c.segs.length - 1].open;
        delete c.segs;
      }

      // Chart series as columns (t = t0 + i * step), which keeps the file small.
      const series = bucket => {
        const n = Math.ceil((to - from) / bucket), cols = { t0: from + bucket / 2, step: bucket, tank: [], set: [], out: [], in: [] };
        const acc = Array.from({ length: n }, () => null);
        for (const s of samples) {
          const i = Math.floor((s.t - from) / bucket);
          const a = acc[i] ||= { tank: [], out: [], in: [], set: null };
          for (const k of ["tank", "out", "in"]) if (s[k] != null) a[k].push(s[k]);
          if (s.set != null) a.set = s.set;
        }
        for (const a of acc) {
          cols.tank.push(a ? reportAvg(a.tank) : null); cols.out.push(a ? reportAvg(a.out) : null);
          cols.in.push(a ? reportAvg(a.in) : null); cols.set.push(a ? a.set : null);
        }
        return cols;
      };
      let maxGap = 0;
      for (let i = 1; i < samples.length; i++) maxGap = Math.max(maxGap, samples[i].t - samples[i - 1].t);

      const keep = ["type", "timestamp", "boot_id", "from_mode", "to_mode", "fault_code", "fault_label", "help_url", "from_version",
        "to_version", "watchdog", "setpoint", "from", "to", "param", "param_name", "unit", "temperature_kind", "from_label", "to_label"];
      const slim = e => Object.fromEntries(keep.filter(k => e[k] != null).map(k => [k, e[k]]));
      return {
        meta: { info: { hostname: info.hostname, version: info.version }, prefs, status, advanced: advanced || { params: {} }, perf, catalog: catalog?.fields || {}, activeFaults: errors?.active || [],
          tz, from, to, generated, interval, telemetryFrom: first ? first.t : null, earliest: latest.earliest,
          coverage: { expected: Math.round((to - Math.max(from, latest.earliest || from)) / interval), got: samples.length,
            connected: samples.filter(s => s.conn).length, maxGap },
          eventsFrom: events.oldest, eventsComplete: events.complete,
          runSummariesFrom: summaries.length ? Math.min(...summaries.map(x => x.start)) : null },
        days, cycles, defrosts, faults, freeze,
        overview: series(300), detail: series(60),
        events: events.list.filter(e => e.timestamp >= from - 3600).map(slim)
      };
    }
    // Event log back to the report start (newest first, 128 per page).
    async function reportEvents(from) {
      const list = [];
      let offset = 0, total = Infinity;
      while (offset < total) {
        const r = await api(`/api/events?offset=${offset}&limit=128`);
        const evs = r.events || [];
        total = r.total || 0;
        list.push(...evs.filter(e => e.timestamp > 0));
        offset += evs.length;
        if (!evs.length || list.length && list[list.length - 1].timestamp < from - 86400) break;
      }
      list.sort((a, b) => a.timestamp - b.timestamp);
      const oldest = list.length ? list[0].timestamp : null;
      return { list, oldest, complete: offset >= total ? true : oldest != null && oldest <= from };
    }
    // The saved page: chart library, data, renderer. "</" is escaped so no
    // string in the data can close the script element early.
    function reportHtml(data) {
      const lib = document.getElementById("tschart-src").textContent;
      const renderer = document.getElementById("report-src").textContent;
      const json = JSON.stringify(data).replace(/<\//g, "<\\/").replace(/\u2028/g, "\\u2028").replace(/\u2029/g, "\\u2029");
      const close = "<" + "/script>";
      const title = `${data.meta.prefs.device_name || data.meta.info.hostname || "Heat pump"} activity report`;
      const icon = document.querySelector('link[rel="icon"]')?.getAttribute("href");
      return `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">`
        + `<title>${esc(title)}</title>${icon ? `<link rel="icon" href="${esc(icon)}">` : ""}</head><body><script>${lib}\nconst DATA = ${json};\n${renderer}${close}</body></html>`;
    }
    // Progress lives in state so a re-render mid-way keeps the button busy.
    function reportProgress(text) {
      state.reportProgress = text;
      const button = document.querySelector('[data-action="download-report"]');
      if (button) { button.textContent = text || "Download report"; button.disabled = !!text; }
    }
    async function downloadReport() {
      if (state.reportProgress) return;
      reportProgress("Starting");
      try {
        const data = await buildReportData(reportProgress);
        reportProgress("Saving");
        const blob = new Blob([reportHtml(data)], { type: "text/html" });
        const name = `${(data.meta.info.hostname || "arctic").replace(/[^\w.-]+/g, "-")}-report-${data.days[data.days.length - 1].date}.html`;
        const a = document.createElement("a");
        a.href = URL.createObjectURL(blob); a.download = name;
        document.body.appendChild(a); a.click(); a.remove();
        setTimeout(() => URL.revokeObjectURL(a.href), 10000);
      } catch (error) {
        toast(`Report failed: ${error.message}`, "bad");
      } finally {
        reportProgress("");
      }
    }
