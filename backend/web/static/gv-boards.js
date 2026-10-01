/* Board refresh + widgets. Depends on GV from gv.js. */
(function () {
  const {
    $, apiGet, apiPost, apiPut, apiSend, loadCreds, saveCreds, readHubForm, credsComplete,
    fmtTs, fmtAge, fmtPc, fmtExcCause, levelClass, levelName, candidateName, operatorName,
    go, phoneSetupLink, setChip,
  } = window.GV;

  const PAGE_SIZE = 25;
  const FETCH_LIMIT = 500;

  /* ---------- Hub ---------- */
  let lastConfigRev = 0;
  let hubBusy = false;

  function drawSpectrum(spec) {
    const canvas = $('spectrumChart');
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const w = canvas.width, h = canvas.height;
    ctx.fillStyle = '#010409';
    ctx.fillRect(0, 0, w, h);
    if (!spec || !spec.bins || !spec.bins.length) {
      $('spectrumMeta').textContent = 'No spectrum data';
      return;
    }
    const bins = spec.bins;
    const max = Math.max(...bins, 1e-6);
    const barW = w / bins.length;
    ctx.fillStyle = '#58a6ff';
    for (let i = 0; i < bins.length; i++) {
      const bh = (bins[i] / max) * (h - 20);
      ctx.fillRect(i * barW, h - bh, Math.max(1, barW - 1), bh);
    }
    if (spec.peak_hz != null) {
      const idx = Math.round(spec.peak_hz / spec.bin_hz);
      ctx.strokeStyle = '#f85149';
      ctx.beginPath();
      ctx.moveTo(idx * barW, 0);
      ctx.lineTo(idx * barW, h);
      ctx.stroke();
    }
    $('spectrumMeta').textContent =
      `seq=${spec.seq} ${spec.sample_hz}Hz bin=${spec.bin_hz.toFixed(2)}Hz peak=${spec.peak_hz?.toFixed(1) ?? '—'}Hz`;
  }

  function renderPresence(dev, gaps) {
    const el = $('presenceLevel');
    const meta = $('presenceMeta');
    const gapEl = $('presenceGaps');
    if (!dev) {
      el.textContent = '—';
      el.className = 'fusion presence-ok';
      meta.textContent = '';
      gapEl.textContent = '';
      return;
    }
    if (dev.silent) {
      el.textContent = 'SILENT';
      el.className = 'fusion presence-silent';
      meta.textContent = `No ESP telemetry/verdicts for ${fmtAge(dev.silent_for_ms)} (threshold 1h). Phone last seen ${fmtTs(dev.last_seen_ms)}.`;
    } else {
      el.textContent = 'LIVE';
      el.className = 'fusion presence-ok';
      meta.textContent = `ESP heartbeat ${fmtAge(dev.silent_for_ms)} ago · last seen ${fmtTs(dev.last_seen_ms)}`;
    }
    gapEl.innerHTML = (!gaps || !gaps.length)
      ? 'No closed ≥1h gaps stored yet.'
      : gaps.slice(0, 6).map((g) => {
          const hrs = (g.gap_ms / 3600000).toFixed(g.gap_ms >= 10 * 3600000 ? 0 : 1);
          return `${fmtTs(g.silent_from_ms)} → ${fmtTs(g.silent_until_ms)} (${hrs}h)`;
        }).join('<br>');
  }

  function renderCrashBrick(crashes, devices) {
    const el = $('crashSummary');
    const meta = $('crashSummaryMeta');
    const card = $('brickCrash');
    if (!crashes.length) {
      el.textContent = 'No crashes';
      el.className = 'fusion lvl-0';
      const total = devices.reduce((s, d) => s + (d.crash_count || 0), 0);
      meta.textContent = total > 0 ? `${total} crash(es) in DB` : 'Tap to open Issues';
      card.classList.add('brick-empty');
      return;
    }
    const c = crashes[0];
    card.classList.remove('brick-empty');
    el.textContent = c.reason || 'crash';
    el.className = 'fusion lvl-2';
    meta.textContent = `${fmtTs(c.ts_ms)} · ${c.device_id} · PC ${fmtPc(c.pc)}`;
  }

  async function loadDeviceConfig() {
    const dev = loadCreds().device;
    if (!dev) return;
    try {
      const row = await apiGet('/devices/' + encodeURIComponent(dev) + '/config');
      lastConfigRev = row.revision || 0;
      $('configMeta').textContent =
        `rev=${row.revision} source=${row.source} ts=${fmtTs(row.ts_ms)}` +
        (row.app_version ? ` app=${row.app_version}` : '');
      $('configEditor').value = JSON.stringify(row.config || {}, null, 2);
    } catch {
      $('configMeta').textContent = 'No config stored yet';
      $('configEditor').value = JSON.stringify({
        schema: 'device.config.v1',
        revision: Math.floor(Date.now() / 1000),
        profile: { power_profile: 2, cpu_mhz: 80, imu_sample_hz: 10 },
        vibro: { schedule_mode: 0, interval_sec: 60, window_sec: 15 },
        mix: { every: 0, ratio: 0 },
      }, null, 2);
    }
  }

  async function saveDeviceConfig() {
    const dev = loadCreds().device;
    if (!dev) return;
    let cfg;
    try { cfg = JSON.parse($('configEditor').value); }
    catch (e) { $('statusBar').textContent = 'Config JSON invalid: ' + e.message; return; }
    const rev = Math.max(lastConfigRev + 1, Math.floor(Date.now() / 1000));
    const row = await apiPut('/devices/' + encodeURIComponent(dev) + '/config', {
      revision: rev, source: 'be', config: cfg,
    });
    lastConfigRev = row.revision;
    $('configMeta').textContent = `Saved rev=${row.revision}`;
    $('statusBar').textContent = 'Config saved — phone/ESP should pull if rev > device cfgseq';
  }

  async function refreshHub() {
    if (hubBusy) return;
    hubBusy = true;
    readHubForm();
    $('statusBar').textContent = 'Loading…';
    try {
      const c = loadCreds();
      const group = encodeURIComponent(c.group || 'default');
      const [devices, fusion] = await Promise.all([
        apiGet('/devices?group_id=' + group),
        apiGet('/groups/' + group + '/status'),
      ]);
      const sel = $('deviceId');
      const prev = sel.value || sel.dataset.pending || c.device || '';
      sel.innerHTML = '';
      devices.forEach((d) => {
        const o = document.createElement('option');
        o.value = d.device_id;
        o.textContent = `${d.device_id} (${d.verdict_count}v/${d.crash_count}c) L${d.latest_level ?? '?'}${d.silent ? ' SILENT' : ''}`;
        sel.appendChild(o);
      });
      if (prev && [...sel.options].some((o) => o.value === prev)) sel.value = prev;
      else if (sel.options.length) sel.value = sel.options[0].value;
      saveCreds({ device: sel.value });
      setChip();

      $('fusionLevel').textContent = levelName(fusion.fused_level);
      $('fusionLevel').className = 'fusion ' + levelClass(fusion.fused_level);
      const silentN = fusion.silent_count || 0;
      $('fusionMeta').textContent =
        `${fusion.reporting_count}/${fusion.device_count} reporting · confidence ${(fusion.confidence * 100).toFixed(0)}%` +
        (silentN ? ` · ${silentN} silent ≥1h` : '');

      const dev = sel.value;
      if (!dev) {
        $('statusBar').textContent = 'No devices in group.';
        return;
      }
      const [spectra, crashes, gaps] = await Promise.all([
        apiGet('/devices/' + encodeURIComponent(dev) + '/spectra?limit=1'),
        apiGet('/devices/' + encodeURIComponent(dev) + '/crashes?limit=5'),
        apiGet('/devices/' + encodeURIComponent(dev) + '/presence_gaps?limit=8'),
      ]);
      renderPresence(devices.find((d) => d.device_id === dev), gaps);
      drawSpectrum(spectra[0] || null);
      renderCrashBrick(crashes, devices);
      await loadDeviceConfig();
      $('statusBar').textContent = `Updated ${new Date().toLocaleTimeString()} · cookie saved`;
    } catch (e) {
      $('statusBar').textContent = 'Error: ' + e.message;
    } finally {
      hubBusy = false;
    }
  }

  function toLocalInput(ms) {
    const d = new Date(ms);
    const pad = (n) => String(n).padStart(2, '0');
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}T${pad(d.getHours())}:${pad(d.getMinutes())}`;
  }
  function exportQuery() {
    const dev = loadCreds().device;
    if (!dev) throw new Error('Select a device first');
    const fromMs = new Date($('exportFrom').value).getTime();
    const toMs = new Date($('exportTo').value).getTime();
    if (!Number.isFinite(fromMs) || !Number.isFinite(toMs)) throw new Error('Set from/to times');
    if (toMs < fromMs) throw new Error('To must be after from');
    return { dev, fromMs, toMs };
  }
  async function fetchExport(format) {
    const { dev, fromMs, toMs } = exportQuery();
    const key = loadCreds().key;
    const q = `?device_id=${encodeURIComponent(dev)}&from_ms=${fromMs}&to_ms=${toMs}&format=${format}&limit=50000`;
    const res = await fetch(window.GV.API + '/export/delivery' + q, { headers: { 'X-API-Key': key } });
    if (!res.ok) throw new Error(await res.text());
    return { text: await res.text(), dev, fromMs, toMs };
  }

  /* ---------- Body ---------- */
  function setKind(kinds, kind, valId, metaId, scale, digits) {
    const latest = kinds && kinds[kind];
    if (!latest) {
      $(valId).textContent = '—';
      $(metaId).textContent = '';
      return;
    }
    let v = latest.value;
    if (v != null && scale) v = v / scale;
    if (v == null) $(valId).textContent = '—';
    else if (digits != null) $(valId).textContent = Number(v).toFixed(digits);
    else $(valId).textContent = String(v);
    const ageMs = Math.max(0, Date.now() - latest.ts_ms);
    $(metaId).textContent = `${latest.source} · ${fmtAge(ageMs)} ago`;
  }

  /* Veepoo SportUtil.getStepLength(heightCm) — metres. */
  function veepooStepLengthM(heightCm) {
    const h = Number(heightCm) || 170;
    let stride;
    if (h < 155) stride = (h * 20) / 42 / 100;
    else if (h < 174) stride = (h * 13) / 28 / 100;
    else stride = (h * 19) / 42 / 100;
    return stride;
  }

  /* H-Band style: steps × stride / 1000 → km (D8 sport metres are a different counter). */
  function setWatchDistanceFromSteps(kinds) {
    const steps = kinds && kinds.steps;
    const unitEl = document.querySelector('#distVal') &&
      document.querySelector('#distVal').parentElement &&
      document.querySelector('#distVal').parentElement.querySelector('.stat-unit');
    if (!steps || steps.value == null) {
      setKind(kinds, 'distance_mm', 'distVal', 'distMeta', 1000, 2);
      if (unitEl) unitEl.textContent = 'm';
      return;
    }
    const km = (Number(steps.value) * veepooStepLengthM(170)) / 1000;
    $('distVal').textContent = km.toFixed(2);
    if (unitEl) unitEl.textContent = 'km';
    const ageMs = Math.max(0, Date.now() - steps.ts_ms);
    const raw = kinds.distance_mm && kinds.distance_mm.value != null
      ? ` · sport ${(Number(kinds.distance_mm.value) / 1000).toFixed(2)} m`
      : '';
    $('distMeta').textContent = `est. from steps · ${fmtAge(ageMs)} ago${raw}`;
  }

  function bodyChartRange() {
    const el = $('bodyRange');
    const v = el ? parseInt(el.value, 10) : 7;
    return (v === 30 || v === 365) ? v : 7;
  }

  function fmtBodyChartVal(v, unit) {
    const n = Number(v) || 0;
    if (unit === 'm') {
      if (Math.abs(n) >= 1000) return (n / 1000).toFixed(1) + 'k';
      return String(Math.round(n));
    }
    if (Math.abs(n) >= 100) return String(Math.round(n));
    return n.toFixed(1);
  }

  /** Causal moving average — smooth day-over-day deltas without looking ahead. */
  function smoothSeries(arr, win) {
    const w = Math.max(1, win | 0);
    const out = new Array(arr.length);
    let run = 0;
    for (let i = 0; i < arr.length; i++) {
      run += arr[i];
      if (i >= w) run -= arr[i - w];
      const n = i + 1 < w ? i + 1 : w;
      out[i] = run / n;
    }
    return out;
  }

  function drawBodyBars(canvasId, days, valueKey, unit, barColor) {
    const canvas = $(canvasId);
    if (!canvas || !days || !days.length) return;
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const cssW = Math.max(canvas.clientWidth || 1100, 320);
    const cssH = 260;
    canvas.width = Math.round(cssW * dpr);
    canvas.height = Math.round(cssH * dpr);
    canvas.style.width = cssW + 'px';
    canvas.style.height = cssH + 'px';
    const ctx = canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

    const padL = 44, padR = 48, padT = 28, padB = 36;
    const w = cssW - padL - padR;
    const h = cssH - padT - padB;
    const vals = days.map((d) => Number(d[valueKey]) || 0);
    const maxV = Math.max(...vals, 1);
    const n = days.length;
    const gap = n > 60 ? 1 : n > 14 ? 2 : 4;
    const barW = Math.max(2, (w - gap * (n - 1)) / n);

    /* Day-over-day delta (today − yesterday); first day = 0. */
    const deltas = vals.map((v, i) => (i === 0 ? 0 : v - vals[i - 1]));
    const smoothWin = n <= 7 ? 2 : n <= 30 ? 3 : 5;
    const smooth = smoothSeries(deltas, smoothWin);
    const absPeak = Math.max(...smooth.map((x) => Math.abs(x)), 1e-6);

    ctx.fillStyle = getComputedStyle(document.documentElement).getPropertyValue('--panel').trim() || '#161b22';
    ctx.fillRect(0, 0, cssW, cssH);
    ctx.strokeStyle = '#30363d';
    ctx.beginPath();
    ctx.moveTo(padL, padT);
    ctx.lineTo(padL, padT + h);
    ctx.lineTo(padL + w, padT + h);
    ctx.stroke();

    ctx.fillStyle = '#8b949e';
    ctx.font = '11px system-ui,sans-serif';
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    for (let i = 0; i <= 4; i++) {
      const y = padT + h - (h * i) / 4;
      const tick = maxV * i / 4;
      ctx.fillText(tick >= 100 ? Math.round(tick).toString() : tick.toFixed(tick < 10 ? 1 : 0), padL - 6, y);
      ctx.strokeStyle = 'rgba(48,54,61,0.7)';
      ctx.beginPath();
      ctx.moveTo(padL, y);
      ctx.lineTo(padL + w, y);
      ctx.stroke();
    }

    /* Right axis: ±smoothed day-over-day delta (zero at mid height). */
    const midY = padT + h / 2;
    ctx.strokeStyle = 'rgba(139,148,158,0.45)';
    ctx.setLineDash([4, 4]);
    ctx.beginPath();
    ctx.moveTo(padL, midY);
    ctx.lineTo(padL + w, midY);
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.fillStyle = '#8b949e';
    ctx.textAlign = 'left';
    ctx.font = '10px system-ui,sans-serif';
    ctx.fillText('+' + fmtBodyChartVal(absPeak, unit), padL + w + 6, padT + 8);
    ctx.fillText('0', padL + w + 6, midY);
    ctx.fillText('−' + fmtBodyChartVal(absPeak, unit), padL + w + 6, padT + h - 8);

    const labelEvery = n <= 7 ? 1 : n <= 30 ? 2 : Math.ceil(n / 12);
    const cx = (i) => padL + i * (barW + gap) + barW / 2;
    const dy = (d) => midY - (d / absPeak) * (h / 2);

    days.forEach((d, i) => {
      const v = vals[i];
      const x = padL + i * (barW + gap);
      const bh = (v / maxV) * h;
      const y = padT + h - bh;
      ctx.fillStyle = barColor;
      ctx.globalAlpha = 0.85;
      ctx.fillRect(x, y, barW, Math.max(bh, v > 0 ? 1 : 0));
      ctx.globalAlpha = 1;

      if (v > 0 && (n <= 31 || barW >= 10)) {
        const label = fmtBodyChartVal(v, unit);
        ctx.save();
        ctx.fillStyle = '#e6edf3';
        ctx.font = (barW >= 18 ? '10px' : '9px') + ' system-ui,sans-serif';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'bottom';
        if (barW < 14 && n > 14) {
          ctx.translate(x + barW / 2, y - 2);
          ctx.rotate(-Math.PI / 2);
          ctx.textAlign = 'left';
          ctx.textBaseline = 'middle';
          ctx.fillText(label, 0, 0);
        } else {
          ctx.fillText(label, x + barW / 2, y - 3);
        }
        ctx.restore();
      }

      if (i % labelEvery === 0 || i === n - 1) {
        const parts = d.day.split('-');
        const tickLabel = n > 60
          ? parts[1] + '/' + parts[2]
          : parts[1] + '-' + parts[2];
        ctx.fillStyle = '#8b949e';
        ctx.font = '10px system-ui,sans-serif';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'top';
        ctx.fillText(tickLabel, x + barW / 2, padT + h + 6);
      }
    });

    /* Smoothed day-over-day delta trendline. */
    ctx.beginPath();
    ctx.strokeStyle = '#f0c14a';
    ctx.lineWidth = 2;
    smooth.forEach((d, i) => {
      const x = cx(i);
      const y = dy(d);
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });
    ctx.stroke();
    smooth.forEach((d, i) => {
      if (i === 0 && n > 1) return;
      ctx.fillStyle = '#f0c14a';
      ctx.beginPath();
      ctx.arc(cx(i), dy(d), n > 60 ? 2 : 3, 0, Math.PI * 2);
      ctx.fill();
    });

    /* Peak callout = latest raw day-over-day delta (today − yesterday). */
    if (n >= 2) {
      const last = n - 1;
      const rawDelta = deltas[last];
      const arrow = rawDelta > 0 ? '↑' : rawDelta < 0 ? '↓' : '→';
      const sign = rawDelta > 0 ? '+' : rawDelta < 0 ? '-' : '';
      const peak = sign + fmtBodyChartVal(Math.abs(rawDelta), unit) + ' ' + arrow;
      const px = cx(last);
      const py = dy(smooth[last]);
      ctx.font = 'bold 11px system-ui,sans-serif';
      ctx.textAlign = 'left';
      ctx.textBaseline = 'bottom';
      const tw = ctx.measureText(peak).width;
      let tx = px + 8;
      let ty = py - 6;
      if (tx + tw > padL + w) tx = px - tw - 8;
      if (ty < padT + 12) ty = py + 14;
      ctx.fillStyle = 'rgba(22,27,34,0.85)';
      ctx.fillRect(tx - 4, ty - 12, tw + 8, 16);
      ctx.fillStyle = rawDelta > 0 ? '#3fb950' : rawDelta < 0 ? '#f85149' : '#e6edf3';
      ctx.fillText(peak, tx, ty);
    }
  }

  async function refreshBodyCharts() {
    const device = loadCreds().device;
    const status = $('bodyChartStatus');
    if (!device) {
      if (status) status.textContent = 'No device in cookie.';
      return;
    }
    const range = bodyChartRange();
    const tz = -new Date().getTimezoneOffset();
    try {
      const data = await apiGet(
        '/wearable/' + encodeURIComponent(device) +
        '/daily?range=' + range + '&tz_offset_min=' + tz
      );
      const days = data.days || [];
      drawBodyBars('bodyDistChart', days, 'distance_m', 'm', '#58a6ff');
      drawBodyBars('bodyKcalChart', days, 'kcal', 'kcal', '#3fb950');
      drawBodyBars('bodyHrChart', days, 'hr_avg', 'bpm', '#f85149');
      drawBodyBars('bodySpo2Chart', days, 'spo2_avg', '%', '#a371f7');
      if (status) {
        const withData = days.filter(
          (d) => d.steps > 0 || d.kcal > 0 || d.hr_avg > 0 || d.spo2_avg > 0
        ).length;
        status.textContent =
          `${range}d · ${withData} day(s) with samples · bars = daily total/avg · gold line = smoothed day-over-day Δ`;
      }
    } catch (e) {
      if (status) status.textContent = String(e.message || e);
    }
  }

  async function refreshBody() {
    const device = loadCreds().device;
    if (!device) { $('bodyStatus').textContent = 'No device in cookie.'; return; }
    try {
      const latest = await apiGet('/wearable/' + encodeURIComponent(device) + '/latest');
      const kinds = latest.kinds || {};
      setKind(kinds, 'hr', 'hrVal', 'hrMeta');
      setKind(kinds, 'spo2', 'spo2Val', 'spo2Meta');
      setKind(kinds, 'steps', 'stepsVal', 'stepsMeta');
      setKind(kinds, 'kcal_x10', 'kcalVal', 'kcalMeta', 10, 1);
      setWatchDistanceFromSteps(kinds);
      setKind(kinds, 'battery_pct', 'batVal', 'batMeta');
      setKind(kinds, 'walk_cm', 'walkVal', 'walkMeta');
      setKind(kinds, 'rssi_esp', 'rssiEspVal', 'rssiEspMeta');
      setKind(kinds, 'rssi_mt200', 'rssiMtVal', 'rssiMtMeta');
      $('bodyStatus').innerHTML = `<span class="lvl-0">live</span> · ${new Date(latest.recv_ms).toLocaleTimeString()}`;
      const rows = await apiGet('/wearable/' + encodeURIComponent(device) + '/samples?limit=40');
      $('bodyRows').innerHTML = rows.map((r) =>
        `<tr><td>${new Date(r.ts_ms).toLocaleTimeString()}</td><td>${r.kind}</td><td>${r.source}</td><td>${r.value ?? ''}</td><td>${r.seq}</td></tr>`
      ).join('');
    } catch (e) {
      $('bodyStatus').textContent = e.message.includes('404')
        ? 'No wearable samples yet — wait for the phone relay.'
        : String(e.message);
    }
    await refreshBodyCharts();
  }

  /* ---------- Vibro ---------- */
  let lastVerdicts = [];
  let verdictPageOffset = 0;

  function renderPaginationBar(containerId, total, pageOffset, onChange) {
    const el = $(containerId);
    if (!el) return;
    el.innerHTML = '';
    if (total <= PAGE_SIZE) return;
    const start = pageOffset + 1;
    const end = Math.min(pageOffset + PAGE_SIZE, total);
    const span = document.createElement('span');
    span.textContent = `Showing ${start}–${end} of ${total}`;
    const prev = document.createElement('button');
    prev.type = 'button';
    prev.textContent = '−25 prev';
    prev.disabled = pageOffset <= 0;
    prev.addEventListener('click', () => onChange(Math.max(0, pageOffset - PAGE_SIZE)));
    const next = document.createElement('button');
    next.type = 'button';
    next.textContent = '+25 next';
    next.disabled = pageOffset + PAGE_SIZE >= total;
    next.addEventListener('click', () => onChange(pageOffset + PAGE_SIZE));
    el.appendChild(prev);
    el.appendChild(next);
    el.appendChild(span);
  }

  function renderVerdicts(verdicts) {
    lastVerdicts = verdicts;
    const tbody = $('verdictRows');
    tbody.innerHTML = '';
    if (verdictPageOffset >= verdicts.length && verdicts.length > 0) {
      verdictPageOffset = Math.max(0, verdicts.length - PAGE_SIZE);
    }
    verdicts.slice(verdictPageOffset, verdictPageOffset + PAGE_SIZE).forEach((v) => {
      const tr = document.createElement('tr');
      tr.innerHTML =
        `<td>${fmtTs(v.ts_ms)}</td><td>${v.seq}</td>` +
        `<td class="${levelClass(v.level)}">${candidateName(v.level)}</td>` +
        `<td>${v.rms?.toFixed(4) ?? '—'}</td><td>${v.peak?.toFixed(4) ?? '—'}</td>` +
        `<td>${v.corr?.toFixed(3) ?? '—'}</td><td>${v.rms_delta?.toFixed(4) ?? '—'}</td>` +
        `<td>${v.edge_score != null ? v.edge_score.toFixed(2) + (v.edge_risk ? ' ' + v.edge_risk : '') : '—'}</td>`;
      tbody.appendChild(tr);
    });
    renderPaginationBar('verdictPagination', verdicts.length, verdictPageOffset, (off) => {
      verdictPageOffset = off;
      renderVerdicts(lastVerdicts);
    });
  }

  function drawTrend(op) {
    const canvas = $('trendChart');
    const ctx = canvas.getContext('2d');
    ctx.fillStyle = '#010409';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    const sensors = (op && op.sensors && op.sensors.length) ? op.sensors : (op ? [op] : []);
    const points = (sensors[0] && sensors[0].points) || (op && op.points) || [];
    if (!points.length) {
      ctx.fillStyle = '#8b949e';
      ctx.fillText('No trend points (repair gaps excluded)', 12, 24);
      return;
    }
    const xs = points.map((p) => p.ts_ms);
    const ys = points.map((p) => p.value);
    const minX = Math.min(...xs), maxX = Math.max(...xs);
    const minY = Math.min(0, ...ys), maxY = Math.max(1, ...ys);
    const pad = 16, w = canvas.width - pad * 2, h = canvas.height - pad * 2;
    ctx.fillStyle = 'rgba(248,81,73,0.18)';
    (op.repairs || []).forEach((r) => {
      const x0 = pad + ((r.started_ms - minX) / (maxX - minX || 1)) * w;
      const x1 = pad + (((r.ended_ms || maxX) - minX) / (maxX - minX || 1)) * w;
      ctx.fillRect(Math.min(x0, x1), pad, Math.max(4, Math.abs(x1 - x0)), h);
    });
    ctx.strokeStyle = '#58a6ff';
    ctx.beginPath();
    points.forEach((p, i) => {
      const x = pad + ((p.ts_ms - minX) / (maxX - minX || 1)) * w;
      const y = pad + h - ((p.value - minY) / (maxY - minY || 1)) * h;
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    });
    ctx.stroke();
  }

  function renderOperator(op) {
    const el = $('operatorLevel');
    if (!op) {
      el.textContent = '—';
      el.className = 'fusion lvl-0';
      $('operatorMeta').textContent = 'No operator_status yet — attach a machine and ingest verdicts.';
      $('ftaPanel').textContent = '';
      $('repairList').textContent = '';
      drawTrend(null);
      return;
    }
    el.textContent = (op.operator_alert ? 'PAGE · ' : '') + operatorName(op.operator_level);
    el.className = 'fusion ' + levelClass(op.operator_level);
    $('operatorMeta').textContent =
      `candidate ${candidateName(op.candidate_level)} · trend ${op.trend || '—'} · score ${(op.score ?? 0).toFixed(2)}` +
      (op.repair_open ? ' · REPAIR OPEN' : '');
    const hints = (op.fta && op.fta.hints) || [];
    const top = (op.fta && op.fta.top_event) || 'FTA';
    $('ftaPanel').innerHTML = `<strong>${top}</strong><br>` + (hints.length
      ? hints.map((h) => `${h.label} (${(h.confidence * 100).toFixed(0)}%) — ${(h.evidence_flags || []).join(', ')}`).join('<br>')
      : 'No IMU-speakable leaf yet.');
    const rows = op.repairs || [];
    $('repairList').innerHTML = rows.length
      ? rows.map((r) => `${fmtTs(r.started_ms)} → ${r.ended_ms ? fmtTs(r.ended_ms) : 'open'} · ${r.fta_leaf || '—'}`).join('<br>')
      : 'No repair windows.';
    drawTrend(op);
  }

  async function refreshVibro() {
    const deviceId = loadCreds().device;
    if (!deviceId) return;
    const [verdicts, machines] = await Promise.all([
      apiGet('/devices/' + encodeURIComponent(deviceId) + '/verdicts?limit=' + FETCH_LIMIT),
      apiGet('/machines').catch(() => []),
    ]);
    renderVerdicts(verdicts);
    const sel = $('machineKey');
    const prev = sel.value;
    sel.innerHTML = '<option value="">— none —</option>';
    machines.forEach((m) => {
      const o = document.createElement('option');
      o.value = m.machine_key;
      o.textContent = `${m.machine_key} · ${m.name} (${m.sensor_count})`;
      sel.appendChild(o);
    });
    const opDev = await apiGet('/devices/' + encodeURIComponent(deviceId) + '/operator_status').catch(() => null);
    if (opDev && opDev.machine_key && [...sel.options].some((o) => o.value === opDev.machine_key)) {
      sel.value = opDev.machine_key;
    } else if (prev && [...sel.options].some((o) => o.value === prev)) {
      sel.value = prev;
    }
    let op = opDev;
    if (sel.value) {
      op = await apiGet('/machines/' + encodeURIComponent(sel.value) + '/operator_status').catch(() => opDev);
    }
    renderOperator(op);
    if (window.GV && window.GV.refreshAiSuggest) window.GV.refreshAiSuggest(true);
  }

  /* ---------- Issues (crashes) ---------- */
  let lastCrashes = [];
  let crashPageOffset = 0;
  let highlightCrashKey = null;
  const CRASH_COL_DEFS = [
    { id: 'time', label: 'Time', min: 72, default: 140 },
    { id: 'device', label: 'Device', min: 64, default: 96 },
    { id: 'seq', label: 'Seq', min: 40, default: 48 },
    { id: 'reason', label: 'Reason', min: 72, default: 120 },
    { id: 'pc', label: 'PC', min: 72, default: 96 },
    { id: 'cause', label: 'Cause', min: 80, default: 160 },
    { id: 'root', label: 'Root cause', min: 120, default: 280 },
    { id: 'thread', label: 'Thread', min: 64, default: 88 },
    { id: 'fw', label: 'FW', min: 64, default: 100 },
    { id: 'uptime', label: 'Uptime', min: 56, default: 72 },
    { id: 'ota', label: 'OTA', min: 64, default: 88 },
    { id: 'boot', label: 'Boot', min: 48, default: 56 },
    { id: 'backtrace', label: 'Backtrace', min: 120, default: 200 },
    { id: 'elf', label: 'ELF', min: 72, default: 88 },
  ];
  function crashRowKey(c) { return `${c.device_id || ''}|${c.seq}|${c.pc ?? ''}|${c.ts_ms ?? ''}`; }
  function loadCrashColPrefs() {
    let order = CRASH_COL_DEFS.map((c) => c.id);
    let widths = {};
    CRASH_COL_DEFS.forEach((c) => { widths[c.id] = c.default; });
    try {
      const savedOrder = JSON.parse(localStorage.getItem('gv_crash_col_order') || 'null');
      const savedWidths = JSON.parse(localStorage.getItem('gv_crash_col_widths') || 'null');
      if (Array.isArray(savedOrder) && savedOrder.every((id) => CRASH_COL_DEFS.some((c) => c.id === id))) {
        const extra = CRASH_COL_DEFS.map((c) => c.id).filter((id) => !savedOrder.includes(id));
        order = savedOrder.concat(extra);
      }
      if (savedWidths && typeof savedWidths === 'object') {
        CRASH_COL_DEFS.forEach((c) => {
          const w = savedWidths[c.id];
          if (typeof w === 'number' && w >= c.min) widths[c.id] = w;
        });
      }
    } catch { /* ignore */ }
    return { order, widths };
  }
  let crashColPrefs = loadCrashColPrefs();
  function saveCrashColPrefs() {
    localStorage.setItem('gv_crash_col_order', JSON.stringify(crashColPrefs.order));
    localStorage.setItem('gv_crash_col_widths', JSON.stringify(crashColPrefs.widths));
  }
  function fmtBacktrace(bt, detail) {
    if (detail && detail.frames && detail.frames.length) {
      return detail.frames.slice(0, 3).map((f) => f.symbol || fmtPc(f.pc)).join('; ');
    }
    if (!bt || !bt.length) return '—';
    return bt.slice(0, 4).map((x) => fmtPc(x)).join(' ');
  }
  function fmtBacktraceFull(bt, detail) {
    if (detail && detail.frames && detail.frames.length) {
      return detail.frames.map((f, i) =>
        `${i}: ${f.symbol || fmtPc(f.pc)}${f.pc != null ? ' @ ' + fmtPc(f.pc) : ''}`
      ).join('\n');
    }
    if (!bt || !bt.length) return '—';
    return bt.map((x, i) => `${i}: ${fmtPc(x)}`).join('\n');
  }
  function otaLabel(c) {
    const d = c.detail || {};
    if (d.ota_outcome) return d.ota_outcome;
    if ((c.soft_reboot_reason || '') === 'fw_upgrade' || /fw_upgrade/.test(c.reason || '')) return c.soft_reboot_reason || c.reason;
    return '—';
  }
  function crashCellHtml(colId, c, showDevice) {
    const d = c.detail || {};
    switch (colId) {
      case 'time': return fmtTs(c.ts_ms);
      case 'device': return showDevice ? (c.device_id || '—') : '—';
      case 'seq': return String(c.seq);
      case 'reason': return c.reason || '—';
      case 'pc': return fmtPc(c.pc);
      case 'cause': return fmtExcCause(c.exccause, c.reason);
      case 'root': {
        const rc = d.root_cause || '';
        if (!rc) return '—';
        return rc.length > 96 ? rc.slice(0, 93) + '…' : rc;
      }
      case 'thread': return c.thread_name || '—';
      case 'fw': return c.fw_version || '—';
      case 'uptime': return c.uptime_ms != null ? (c.uptime_ms / 1000).toFixed(1) + 's' : '—';
      case 'ota': return otaLabel(c);
      case 'boot': return d.boot_part || '—';
      case 'backtrace': return fmtBacktrace(c.backtrace, c.detail);
      case 'elf': {
        const fw = c.fw_version || '';
        if (!fw) return '—';
        const matched = d.elf_matched ? ' ✓' : '';
        return `<button type="button" class="crash-elf-upload" data-fw="${fw.replace(/"/g, '&quot;')}" title="Upload zephyr.elf for ${fw}">Upload${matched}</button>`;
      }
      default: return '—';
    }
  }
  function crashDetailText(c) {
    const d = c.detail || {};
    const lines = [
      `device_id: ${c.device_id}`,
      `group_id: ${c.group_id || '—'}`,
      `ts_ms: ${c.ts_ms} (${fmtTs(c.ts_ms)})`,
      `seq: ${c.seq}`,
      `reason: ${c.reason || '—'}`,
      `pc: ${fmtPc(c.pc)}`,
      `exccause: ${fmtExcCause(c.exccause, c.reason)}`,
      `thread: ${c.thread_name || '—'}`,
      `fw_version: ${c.fw_version || '—'}`,
      `elf: ${d.elf || '—'} matched=${d.elf_matched ? 'yes' : 'no'} ${d.elf_url || ''}`,
      `elf_note: ${d.elf_note || '—'}`,
      `reset_reason: ${c.reset_reason ?? '—'}`,
      `uptime_ms: ${c.uptime_ms ?? '—'}`,
      `root_cause: ${d.root_cause || '—'}`,
      `ota_outcome: ${d.ota_outcome || '—'}`,
      `boot_part: ${d.boot_part || '—'}`,
      `target_part: ${d.target_part || '—'}`,
      '',
      'Backtrace / symbols:',
      fmtBacktraceFull(c.backtrace, c.detail),
    ];
    if (c.detail && Object.keys(c.detail).length) {
      lines.push('', 'detail JSON:', JSON.stringify(c.detail, null, 2));
    }
    return lines.join('\n');
  }
  function showCrashModal(c) {
    $('crashModalBody').textContent = crashDetailText(c);
    $('crashModal').showModal();
  }
  function renderCrashes(crashes, showDevice) {
    const tbody = $('crashRows');
    const hint = $('crashHint');
    tbody.innerHTML = '';
    lastCrashes = crashes;
    if (!crashes.length) {
      hint.style.display = 'block';
      renderPaginationBar('crashPagination', 0, 0, () => {});
      return;
    }
    hint.style.display = 'none';
    if (crashPageOffset >= crashes.length) {
      crashPageOffset = Math.max(0, crashes.length - PAGE_SIZE);
    }
    crashes.slice(crashPageOffset, crashPageOffset + PAGE_SIZE).forEach((c) => {
      const key = crashRowKey(c);
      const tr = document.createElement('tr');
      tr.className = 'crash-row';
      tr.dataset.crashKey = key;
      tr.innerHTML = crashColPrefs.order.map((colId) => {
        const cls = (colId === 'pc' || colId === 'backtrace') ? ' class="crash-pc"' : '';
        return `<td${cls}>${crashCellHtml(colId, c, showDevice)}</td>`;
      }).join('');
      tr.addEventListener('click', (ev) => {
        if (ev.target && ev.target.closest && ev.target.closest('.crash-elf-upload')) return;
        showCrashModal(c);
      });
      const elfBtn = tr.querySelector('.crash-elf-upload');
      if (elfBtn) {
        elfBtn.addEventListener('click', (ev) => {
          ev.preventDefault();
          ev.stopPropagation();
          uploadCrashElf(elfBtn.getAttribute('data-fw') || c.fw_version || '');
        });
      }
      if (highlightCrashKey === key) tr.classList.add('crash-row-highlight');
      tbody.appendChild(tr);
    });
    renderPaginationBar('crashPagination', crashes.length, crashPageOffset, (off) => {
      crashPageOffset = off;
      renderCrashes(lastCrashes, showDevice);
    });
  }
  function buildCrashThead() {
    const thead = $('crashThead');
    const colgroup = $('crashColgroup');
    if (!thead || !colgroup) return;
    thead.innerHTML = '';
    colgroup.innerHTML = '';
    const tr = document.createElement('tr');
    crashColPrefs.order.forEach((colId, idx) => {
      const def = CRASH_COL_DEFS.find((c) => c.id === colId);
      if (!def) return;
      const col = document.createElement('col');
      col.style.width = crashColPrefs.widths[colId] + 'px';
      colgroup.appendChild(col);
      const th = document.createElement('th');
      th.textContent = def.label;
      th.draggable = true;
      th.dataset.colId = colId;
      th.addEventListener('dragstart', (e) => {
        th.classList.add('dragging');
        e.dataTransfer.setData('text/plain', colId);
        e.dataTransfer.effectAllowed = 'move';
      });
      th.addEventListener('dragend', () => th.classList.remove('dragging'));
      th.addEventListener('dragover', (e) => { e.preventDefault(); th.classList.add('drag-over'); });
      th.addEventListener('dragleave', () => th.classList.remove('drag-over'));
      th.addEventListener('drop', (e) => {
        e.preventDefault();
        th.classList.remove('drag-over');
        const fromId = e.dataTransfer.getData('text/plain');
        if (!fromId || fromId === colId) return;
        const order = [...crashColPrefs.order];
        const fromIdx = order.indexOf(fromId);
        const toIdx = order.indexOf(colId);
        if (fromIdx < 0 || toIdx < 0) return;
        order.splice(fromIdx, 1);
        order.splice(toIdx, 0, fromId);
        crashColPrefs.order = order;
        saveCrashColPrefs();
        buildCrashThead();
        renderCrashes(lastCrashes, document.querySelector('input[name="crashScope"]:checked')?.value === 'group');
      });
      if (idx < crashColPrefs.order.length - 1) {
        const handle = document.createElement('span');
        handle.className = 'col-resize-handle';
        handle.addEventListener('mousedown', (e) => {
          e.preventDefault();
          e.stopPropagation();
          const startX = e.clientX;
          const startW = crashColPrefs.widths[colId];
          function onMove(ev) {
            crashColPrefs.widths[colId] = Math.max(def.min, startW + (ev.clientX - startX));
            col.style.width = crashColPrefs.widths[colId] + 'px';
          }
          function onUp() {
            document.removeEventListener('mousemove', onMove);
            document.removeEventListener('mouseup', onUp);
            saveCrashColPrefs();
          }
          document.addEventListener('mousemove', onMove);
          document.addEventListener('mouseup', onUp);
        });
        th.appendChild(handle);
      }
      tr.appendChild(th);
    });
    thead.appendChild(tr);
  }

  async function uploadCrashElf(fwVersion) {
    const fw = (fwVersion || '').trim();
    if (!fw) {
      alert('Row has no fw_version');
      return;
    }
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.elf,application/octet-stream';
    input.style.display = 'none';
    document.body.appendChild(input);
    input.addEventListener('change', async () => {
      const file = input.files && input.files[0];
      input.remove();
      if (!file) return;
      const key = loadCreds().key;
      if (!key) {
        alert('API key required');
        return;
      }
      const fd = new FormData();
      fd.append('file', file, file.name || 'zephyr.elf');
      try {
        const res = await fetch(
          window.GV.API + '/firmware-elfs/' + encodeURIComponent(fw),
          { method: 'POST', headers: { 'X-API-Key': key }, body: fd },
        );
        if (!res.ok) throw new Error(await res.text());
        const out = await res.json();
        alert(
          `ELF stored for ${out.fw_version}\n` +
          `re-symbolicated ${out.resymbolicated} crash(es)\n` +
          `${out.url}`,
        );
        await refreshIssues();
      } catch (e) {
        alert('ELF upload failed: ' + (e && e.message ? e.message : e));
      }
    });
    input.click();
  }

  async function refreshIssues(opts) {
    // Autoupdate / re-entry: keep the board usable. Only show the loading banner
    // on cold load (empty table) or when explicitly requested.
    const forceLoading = !!(opts && opts.forceLoading);
    const silent = !forceLoading && lastCrashes.length > 0;
    const loading = $('crashLoading');
    const tableScroll = document.querySelector('#crashReportsSection .table-scroll');
    if (!silent) {
      if (loading) loading.hidden = false;
      if (tableScroll) tableScroll.style.opacity = '0.35';
    }
    try {
      const c = loadCreds();
      const groupScope = document.querySelector('input[name="crashScope"]:checked')?.value === 'group';
      const crashPath = groupScope
        ? '/crashes?group_id=' + encodeURIComponent(c.group || 'default') + '&limit=' + FETCH_LIMIT
        : '/devices/' + encodeURIComponent(c.device) + '/crashes?limit=' + FETCH_LIMIT;
      const crashes = await apiGet(crashPath);
      renderCrashes(crashes, groupScope);
    } catch (e) {
      const hint = $('crashHint');
      if (hint) {
        hint.style.display = 'block';
        hint.textContent = 'Failed to load crashes: ' + (e && e.message ? e.message : e);
      }
    } finally {
      if (loading) loading.hidden = true;
      if (tableScroll) tableScroll.style.opacity = '';
    }
  }

  /* ---------- Insights ---------- */
  function num(v, digits) {
    if (v == null || Number.isNaN(v)) return '—';
    return typeof v === 'number' ? v.toFixed(digits) : String(v);
  }
  function fillInsDeviceSelect(devices, selected) {
    const sel = $('insDeviceSelect');
    if (!sel) return;
    const prev = selected || sel.value || '';
    sel.innerHTML = '';
    (devices || []).forEach((d) => {
      const o = document.createElement('option');
      o.value = d.device_id;
      o.textContent = `${d.device_id}${d.silent ? ' (SILENT)' : ' (LIVE)'}`;
      sel.appendChild(o);
    });
    if (prev && [...sel.options].some((o) => o.value === prev)) sel.value = prev;
    else if (sel.options.length) sel.value = sel.options[0].value;
  }
  function showInsSilentWarn(ins, devices) {
    const el = $('insSilentWarn');
    if (!el) return;
    if (!ins || !ins.silent) {
      el.classList.remove('visible');
      el.textContent = '';
      return;
    }
    const live = (devices || []).filter((d) => !d.silent && d.device_id !== ins.device_id);
    if (!live.length) {
      el.textContent =
        `Selected device ${ins.device_id} is SILENT (no ESP traffic). Phone/ESP may be on another id — pick it above.`;
    } else {
      const ids = live.map((d) => d.device_id).join(', ');
      el.innerHTML =
        `Selected <code>${ins.device_id}</code> is SILENT. Live in this group: <strong>${ids}</strong>. ` +
        `<button type="button" id="insSwitchLiveBtn">Switch to ${live[0].device_id}</button>`;
      $('insSwitchLiveBtn')?.addEventListener('click', () => {
        saveCreds({ device: live[0].device_id });
        const hub = $('deviceId');
        if (hub) hub.value = live[0].device_id;
        setChip();
        refreshInsights();
      });
    }
    el.classList.add('visible');
  }
  async function refreshInsights() {
    const c = loadCreds();
    let device = c.device;
    if (!device) {
      $('insightsStatus').textContent = 'No device in cookie — pick one on Hub or below.';
      return;
    }
    try {
      const group = encodeURIComponent(c.group || 'default');
      const [devices, ins] = await Promise.all([
        apiGet('/devices?group_id=' + group),
        apiGet('/devices/' + encodeURIComponent(device) + '/insights'),
      ]);
      fillInsDeviceSelect(devices, device);
      showInsSilentWarn(ins, devices);
      $('insBat').textContent = ins.battery_pct != null ? String(ins.battery_pct) : '—';
      const batBits = [];
      if (ins.voltage != null) batBits.push(ins.voltage.toFixed(2) + ' V');
      if (ins.voltage != null && ins.voltage >= 4.50) batBits.push('DC / USB');
      else if (ins.battery_pct != null && ins.voltage != null) batBits.push('STATUS');
      else if (ins.voltage != null) batBits.push('LiPo approx');
      else batBits.push('no STATUS voltage yet');
      $('insBatMeta').textContent = batBits.join(' · ');
      $('insUptime').textContent = ins.last_uptime_ms != null ? (ins.last_uptime_ms / 1000).toFixed(1) : '—';
      $('insUptimeMeta').textContent = ins.last_uptime_ms != null ? 'from last crash / soft report' : 'no report yet';
      $('insRssiEsp').textContent = num(ins.avg_rssi_esp, 0);
      $('insRssiEspMeta').textContent = ins.last_rssi_esp != null ? `last ${ins.last_rssi_esp.toFixed(0)} dBm` : '';
      $('insRssiMt').textContent = num(ins.avg_rssi_mt200, 0);
      $('insRssiMtMeta').textContent = ins.last_rssi_mt200 != null ? `last ${ins.last_rssi_mt200.toFixed(0)} dBm` : '';
      $('insTemp').textContent = num(ins.chip_temp_c, 1);
      $('insCpu').textContent = ins.cpu_mhz != null ? ins.cpu_mhz + ' MHz' : '—';
      $('insCpuMeta').textContent = ins.apb_mhz != null ? `APB ${ins.apb_mhz} MHz · spool free ${ins.spool_free_b ?? '—'}` : '';
      $('insPresence').textContent = ins.silent ? 'SILENT' : 'LIVE';
      $('insPresence').className = 'stat-value ' + (ins.silent ? 'presence-silent' : 'presence-ok');
      $('insPresenceMeta').textContent = `last ESP ${fmtAge(ins.silent_for_ms)} ago · phone ${fmtTs(ins.last_seen_ms)}`;
      $('insFw').textContent = ins.latest_fw || '—';
      $('insFwMeta').textContent =
        (ins.latest_fwc != null ? `fwc ${ins.latest_fwc} · ` : '') +
        `${ins.verdict_count} verdicts · ${ins.crash_count} crashes · highest fwc wins`;
      const fmtKb = (kb) => {
        if (kb == null) return '—';
        if (kb >= 1024) return (kb / 1024).toFixed(kb >= 10240 ? 0 : 1) + ' MB';
        return kb + ' kB';
      };
      $('insBus').textContent =
        (ins.spi_mhz != null ? `SPI ${ins.spi_mhz} MHz` : 'SPI —') +
        (ins.i2c_khz != null ? ` · I2C ${ins.i2c_khz} kHz` : '');
      $('insBusMeta').textContent = 'from STATUS telemetry';
      const fmtRate = (bps) => {
        if (bps == null) return null;
        if (bps >= 1024) return (bps / 1024).toFixed(bps >= 10240 ? 0 : 1) + ' kB/s';
        return bps + ' B/s';
      };
      const txRate = fmtRate(ins.ble_tx_bps);
      const rxRate = fmtRate(ins.ble_rx_bps);
      if (txRate != null) {
        $('insBle').textContent = `TX ${txRate}`;
        $('insBleMeta').textContent = `RX ${rxRate ?? '—'} · since last STATUS`;
      } else {
        $('insBle').textContent = `TX ${fmtKb(ins.ble_tx_kb)}`;
        $('insBleMeta').textContent = `RX ${fmtKb(ins.ble_rx_kb)} · no rate yet`;
      }
      $('insDisp').textContent =
        ins.display_on == null ? '—' : (ins.display_on ? 'ON' : 'OFF');
      $('insDispMeta').textContent = 'STATUS scr';
      if (ins.wifi_on) {
        $('insWifi').textContent =
          (ins.wifi_rssi != null && ins.wifi_rssi > -120)
            ? `${ins.wifi_rssi} dBm` : 'ON';
        const bits = [];
        if (ins.wifi_ssid) bits.push(ins.wifi_ssid);
        bits.push(ins.wifi_ap ? 'AP on' : 'STA');
        $('insWifiMeta').textContent = bits.join(' · ');
      } else {
        $('insWifi').textContent = ins.wifi_on == null ? '—' : 'OFF';
        $('insWifiMeta').textContent = ins.wifi_ap ? 'AP on' : '';
      }
      const ota = ins.ota;
      if (!ota) {
        $('insOtaLatest').textContent = 'No OTA report yet';
        $('insOtaLatest').className = 'fusion lvl-0';
        $('insOtaMeta').textContent = 'A fw_upgrade soft report appears after the next cloud OTA once the phone reconnects and drains the crash ring.';
      } else {
        const ok = ota.ok === true;
        const nok = ota.ok === false;
        $('insOtaLatest').textContent = (ok ? 'OK' : nok ? 'NOK' : 'OTA') + (ota.outcome ? ' · ' + ota.outcome : '');
        $('insOtaLatest').className = 'fusion ' + (ok ? 'lvl-0' : nok ? 'lvl-2' : 'lvl-1');
        $('insOtaMeta').textContent =
          `${fmtTs(ota.ts_ms)} · boot ${ota.boot_part || '?'} → ${ota.target_part || '?'} · ${ota.fw_version || ''}`;
      }
      $('insOtaRows').innerHTML = (ins.recent_ota || []).map((o) => {
        const cls = o.ok === true ? 'ota-ok' : o.ok === false ? 'ota-nok' : '';
        const res = o.ok === true ? 'OK' : o.ok === false ? 'NOK' : '—';
        return `<tr><td>${fmtTs(o.ts_ms)}</td><td class="${cls}">${res}</td><td>${o.outcome || '—'}</td>` +
          `<td>${o.boot_part || '—'}</td><td>${o.target_part || '—'}</td><td>${o.fw_version || '—'}</td>` +
          `<td>${o.uptime_ms != null ? (o.uptime_ms / 1000).toFixed(1) + 's' : '—'}</td></tr>`;
      }).join('') || '<tr><td colspan="7" style="color:var(--muted)">No OTA rows</td></tr>';
      $('insightsStatus').textContent = `Updated ${new Date().toLocaleTimeString()}`;
    } catch (e) {
      $('insightsStatus').textContent = 'Error: ' + e.message;
    }
  }

  /* ---------- AHRS cube + map ---------- */
  let glReady = false, currentRot = [1, 0, 0, 0, 1, 0, 0, 0, 1];
  let lastRecvMs = 0;
  let leafletMap = null, leafletOk = false;
  let gpsLine = { setLatLngs() {} }, imuLine = { setLatLngs() {} }, osrmLine = { setLatLngs() {} };
  let originMarker = null, imuOriginMarker = null, framedOnce = false;
  let lastGpsPoints = [], lastImuPoints = [], lastOsrmKey = '';
  let ahrsAgeTimer = null;

  function setLink(cls, text) {
    const el = $('statLink');
    if (!el) return;
    el.className = 'badge ' + cls;
    el.textContent = text;
  }
  function compileShader(gl, src, type) {
    const sh = gl.createShader(type);
    gl.shaderSource(sh, src);
    gl.compileShader(sh);
    if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(sh));
    return sh;
  }
  function initGl() {
    const canvas = $('glCanvas');
    if (!canvas) return;
    const gl = canvas.getContext('webgl');
    if (!gl) { setLink('alert', 'no WebGL'); return; }
    const vs = `attribute vec3 aPos; attribute vec3 aCol; uniform mat3 uRot; varying vec3 vCol;
      void main() { vec3 p = uRot * aPos; float z = p.z * 0.35 + 2.2; gl_Position = vec4(p.x/z, p.y/z, p.z*0.2, 1.0); vCol = aCol; }`;
    const fs = `precision mediump float; varying vec3 vCol; void main() { gl_FragColor = vec4(vCol, 1.0); }`;
    const program = gl.createProgram();
    gl.attachShader(program, compileShader(gl, vs, gl.VERTEX_SHADER));
    gl.attachShader(program, compileShader(gl, fs, gl.FRAGMENT_SHADER));
    gl.linkProgram(program);
    gl.useProgram(program);
    const posLoc = gl.getAttribLocation(program, 'aPos');
    const colLoc = gl.getAttribLocation(program, 'aCol');
    const matLoc = gl.getUniformLocation(program, 'uRot');
    const s = 0.6;
    const faces = [
      [[ s,-s,-s],[ s, s,-s],[ s, s, s],[ s,-s,-s],[ s, s, s],[ s,-s, s]], [0.97,0.32,0.29],
      [[-s,-s, s],[-s, s, s],[-s, s,-s],[-s,-s, s],[-s, s,-s],[-s,-s,-s]], [0.45,0.12,0.10],
      [[-s, s,-s],[-s, s, s],[ s, s, s],[-s, s,-s],[ s, s, s],[ s, s,-s]], [0.25,0.73,0.31],
      [[-s,-s, s],[-s,-s,-s],[ s,-s,-s],[-s,-s, s],[ s,-s,-s],[ s,-s, s]], [0.10,0.30,0.12],
      [[-s,-s, s],[ s,-s, s],[ s, s, s],[-s,-s, s],[ s, s, s],[-s, s, s]], [0.35,0.65,1.00],
      [[ s,-s,-s],[-s,-s,-s],[-s, s,-s],[ s,-s,-s],[-s, s,-s],[ s, s,-s]], [0.10,0.20,0.45],
    ];
    const verts = [];
    for (let i = 0; i < faces.length; i += 2) {
      const pts = faces[i], col = faces[i + 1];
      for (const p of pts) verts.push(p[0], p[1], p[2], col[0], col[1], col[2]);
    }
    const vbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(verts), gl.STATIC_DRAW);
    gl.enable(gl.DEPTH_TEST);
    const vertexCount = verts.length / 6;
    glReady = true;
    function render() {
      const dpr = Math.min(window.devicePixelRatio || 1, 2);
      const w = canvas.clientWidth * dpr, h = canvas.clientHeight * dpr;
      if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
      gl.viewport(0, 0, canvas.width, canvas.height);
      gl.clearColor(0.004, 0.008, 0.035, 1);
      gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
      gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
      gl.enableVertexAttribArray(posLoc);
      gl.vertexAttribPointer(posLoc, 3, gl.FLOAT, false, 24, 0);
      gl.enableVertexAttribArray(colLoc);
      gl.vertexAttribPointer(colLoc, 3, gl.FLOAT, false, 24, 12);
      const r = currentRot;
      gl.uniformMatrix3fv(matLoc, false, [r[0], r[3], r[6], r[1], r[4], r[7], r[2], r[5], r[8]]);
      gl.drawArrays(gl.TRIANGLES, 0, vertexCount);
      requestAnimationFrame(render);
    }
    requestAnimationFrame(render);
  }
  function eulerFromRot(r) {
    const pitch = Math.asin(Math.max(-1, Math.min(1, -r[6])));
    const roll = Math.atan2(r[7], r[8]);
    const yaw = Math.atan2(r[3], r[0]);
    const d = (x) => (x * 180 / Math.PI).toFixed(1) + '°';
    return [d(roll), d(pitch), d(yaw)];
  }
  function gpsOn() { return $('layerGps').checked; }
  function imuOn() { return $('layerImu').checked; }
  function setOverlayVisible(layer, on) {
    if (!leafletOk || !leafletMap || !layer || !layer.addTo) return;
    const has = leafletMap.hasLayer(layer);
    if (on && !has) layer.addTo(leafletMap);
    if (!on && has) leafletMap.removeLayer(layer);
  }
  function applyLayers() {
    setOverlayVisible(gpsLine, gpsOn());
    setOverlayVisible(imuLine, imuOn());
    setOverlayVisible(osrmLine, gpsOn());
    if (originMarker) setOverlayVisible(originMarker, gpsOn() || imuOn());
    if (imuOriginMarker) setOverlayVisible(imuOriginMarker, imuOn());
  }
  function initLeaflet() {
    if (typeof L === 'undefined' || leafletOk) return;
    leafletMap = L.map('map').setView([0, 0], 2);
    L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
      maxZoom: 19,
      attribution: '&copy; OpenStreetMap',
    }).addTo(leafletMap);
    gpsLine = L.polyline([], { color: '#3fb950', weight: 5, opacity: 0.95 }).addTo(leafletMap);
    imuLine = L.polyline([], { color: '#f85149', weight: 4, dashArray: '8 5', opacity: 0.95 }).addTo(leafletMap);
    osrmLine = L.polyline([], { color: '#a371f7', weight: 3, opacity: 0.7 }).addTo(leafletMap);
    leafletOk = true;
    applyLayers();
  }
  function haversineM(a, b) {
    const R = 6371000;
    const dLat = (b[0] - a[0]) * Math.PI / 180;
    const dLon = (b[1] - a[1]) * Math.PI / 180;
    const lat1 = a[0] * Math.PI / 180, lat2 = b[0] * Math.PI / 180;
    const h = Math.sin(dLat / 2) ** 2 + Math.cos(lat1) * Math.cos(lat2) * Math.sin(dLon / 2) ** 2;
    return 2 * R * Math.asin(Math.min(1, Math.sqrt(h)));
  }
  function pathLengthM(points) {
    let d = 0;
    for (let i = 1; i < points.length; i++) d += haversineM(points[i - 1], points[i]);
    return d;
  }
  function fmtM(m) {
    if (m == null) return '—';
    return m >= 1000 ? (m / 1000).toFixed(2) + ' km' : m.toFixed(0) + ' m';
  }
  async function maybeFetchOsrm(gpsPoints) {
    if (gpsPoints.length < 2) return;
    const start = gpsPoints[0], end = gpsPoints[gpsPoints.length - 1];
    const key = `${start[0].toFixed(5)},${start[1].toFixed(5)}->${end[0].toFixed(5)},${end[1].toFixed(5)}`;
    if (key === lastOsrmKey) return;
    lastOsrmKey = key;
    try {
      const url = `https://router.project-osrm.org/route/v1/foot/${start[1]},${start[0]};${end[1]},${end[0]}?overview=full&geometries=geojson`;
      const res = await fetch(url);
      if (!res.ok) return;
      const data = await res.json();
      const route = data.routes && data.routes[0];
      if (!route) return;
      osrmLine.setLatLngs(route.geometry.coordinates.map(([lon, lat]) => [lat, lon]));
      $('statOsrmDist').textContent = fmtM(route.distance);
    } catch { /* public demo */ }
  }
  async function refreshAhrs() {
    if (!glReady) initGl();
    if (!leafletOk) {
      try { initLeaflet(); } catch { setLink('alert', 'map init failed'); }
    } else if (leafletMap) {
      leafletMap.invalidateSize();
    }
    if (!ahrsAgeTimer) {
      ahrsAgeTimer = setInterval(() => {
        if (!lastRecvMs) { $('statAge').textContent = '—'; return; }
        const ageMs = Date.now() - lastRecvMs;
        $('statAge').textContent = (ageMs / 1000).toFixed(1) + 's';
        if (ageMs > 5000) setLink('alert', 'stale');
      }, 500);
    }
    const c = loadCreds();
    if (!c.device) { setLink('warn', 'need device'); return; }
    try {
      const sample = await apiGet('/ahrs/' + encodeURIComponent(c.device) + '/latest');
      currentRot = sample.rot;
      lastRecvMs = Date.now();
      setLink('ok', 'live');
      $('statSeq').textContent = sample.seq;
      const [roll, pitch, yaw] = eulerFromRot(sample.rot);
      $('statRoll').textContent = roll;
      $('statPitch').textContent = pitch;
      $('statYaw').textContent = yaw;
    } catch (e) {
      if (String(e.message).includes('404')) setLink('warn', 'no sample yet');
      else setLink('alert', 'AHRS fetch failed');
    }
    try {
      const route = await apiGet('/geo/' + encodeURIComponent(c.device) + '/route');
      const gpsPoints = (route.gps || []).map((p) => [p.lat, p.lon]);
      const imuPoints = (route.imu || []).map((p) => [p.lat, p.lon]);
      lastGpsPoints = gpsPoints;
      lastImuPoints = imuPoints;
      gpsLine.setLatLngs(gpsOn() ? gpsPoints : []);
      imuLine.setLatLngs(imuOn() ? imuPoints : []);
      applyLayers();
      $('statGpsN').textContent = gpsPoints.length;
      $('statImuN').textContent = imuPoints.length;
      $('statGpsDist').textContent = fmtM(pathLengthM(gpsPoints));
      $('statImuDist').textContent = fmtM(pathLengthM(imuPoints));
      if (leafletOk && (gpsPoints.length || imuPoints.length)) {
        const origin = gpsPoints[0] || imuPoints[0];
        if (origin) {
          if (!originMarker) originMarker = L.marker(origin).bindPopup('START — first GPS');
          else originMarker.setLatLng(origin);
          if (!imuOriginMarker) {
            imuOriginMarker = L.circleMarker(origin, {
              radius: 6, color: '#f85149', fillColor: '#f85149', fillOpacity: 0.9, weight: 2,
            });
          } else imuOriginMarker.setLatLng(origin);
          applyLayers();
        }
        if (!framedOnce && leafletMap) {
          const all = [];
          if (gpsOn()) all.push(...gpsPoints);
          if (imuOn()) all.push(...imuPoints);
          if (all.length > 1) leafletMap.fitBounds(L.latLngBounds(all), { padding: [30, 30] });
          else if (all.length === 1) leafletMap.setView(all[0], 17);
          framedOnce = true;
        }
        if (gpsOn()) maybeFetchOsrm(gpsPoints);
      }
    } catch { /* geo optional */ }
  }

  /* ---------- wire ---------- */
  window.GV.onShow.hub = refreshHub;
  window.GV.onShow.body = refreshBody;
  window.GV.onShow.vibro = refreshVibro;
  window.GV.onShow.ahrs = refreshAhrs;
  window.GV.onShow.insights = refreshInsights;
  window.GV.onShow.issues = refreshIssues;

  document.addEventListener('DOMContentLoaded', () => {
    buildCrashThead();
    const now = Date.now();
    if ($('exportFrom')) $('exportFrom').value = toLocalInput(now - 24 * 3600 * 1000);
    if ($('exportTo')) $('exportTo').value = toLocalInput(now);
    $('bodyRange')?.addEventListener('change', () => { refreshBodyCharts(); });
    window.addEventListener('resize', () => {
      if (document.querySelector('.board.active[data-board="body"]')) {
        refreshBodyCharts();
      }
    });
    document.querySelectorAll('.brick[data-go]').forEach((el) => {
      el.addEventListener('click', () => go(el.dataset.go));
    });
    $('refreshBtn')?.addEventListener('click', refreshHub);
    $('configLoadBtn')?.addEventListener('click', () => loadDeviceConfig().catch((e) => {
      $('statusBar').textContent = 'Error: ' + e.message;
    }));
    $('configSaveBtn')?.addEventListener('click', () => saveDeviceConfig().catch((e) => {
      $('statusBar').textContent = 'Error: ' + e.message;
    }));
    $('copyApiKeyBtn')?.addEventListener('click', async () => {
      const key = loadCreds().key;
      if (!key) return;
      try { await navigator.clipboard.writeText(key); $('statusBar').textContent = 'API key copied.'; }
      catch { $('statusBar').textContent = 'Copy failed.'; }
    });
    $('phoneSetupBtn')?.addEventListener('click', async () => {
      try {
        await navigator.clipboard.writeText(phoneSetupLink());
        $('statusBar').textContent = 'Phone setup link copied.';
      } catch (e) { $('statusBar').textContent = 'Error: ' + e.message; }
    });
    $('exportCsvBtn')?.addEventListener('click', async () => {
      $('exportStatus').textContent = 'Exporting CSV…';
      try {
        const { text, dev, fromMs, toMs } = await fetchExport('csv');
        const a = document.createElement('a');
        a.href = URL.createObjectURL(new Blob([text], { type: 'text/csv;charset=utf-8' }));
        a.download = `delivery-${dev}-${fromMs}-${toMs}.csv`;
        a.click();
        URL.revokeObjectURL(a.href);
        $('exportStatus').textContent = `Downloaded ${text.split('\n').filter((l) => l.trim()).length - 1} rows.`;
      } catch (e) { $('exportStatus').textContent = 'Error: ' + e.message; }
    });
    $('exportSheetsBtn')?.addEventListener('click', async () => {
      $('exportStatus').textContent = 'Copying TSV…';
      try {
        const { text } = await fetchExport('tsv');
        await navigator.clipboard.writeText(text);
        window.open('https://docs.google.com/spreadsheets/create', '_blank', 'noopener');
        $('exportStatus').textContent = `Copied ${text.split('\n').filter((l) => l.trim()).length - 1} rows. Paste into Sheets.`;
      } catch (e) { $('exportStatus').textContent = 'Error: ' + e.message; }
    });
    $('createMachineBtn')?.addEventListener('click', async () => {
      try {
        const key = $('newMachineKey').value.trim();
        if (!key) throw new Error('machine key required');
        await apiPost('/machines', { machine_key: key, name: $('newMachineName').value.trim() || key, kind: 'pump' });
        await refreshVibro();
      } catch (e) { $('operatorMeta').textContent = 'Error: ' + e.message; }
    });
    $('attachSensorBtn')?.addEventListener('click', async () => {
      try {
        const key = $('machineKey').value || $('newMachineKey').value.trim();
        const dev = loadCreds().device;
        if (!key || !dev) throw new Error('select machine and device');
        await apiPost('/machines/' + encodeURIComponent(key) + '/sensors', { device_id: dev });
        await refreshVibro();
      } catch (e) { $('operatorMeta').textContent = 'Error: ' + e.message; }
    });
    $('machineKey')?.addEventListener('change', async () => {
      if (!$('machineKey').value) return;
      try { renderOperator(await apiGet('/machines/' + encodeURIComponent($('machineKey').value) + '/operator_status')); }
      catch (e) { $('operatorMeta').textContent = 'Error: ' + e.message; }
      if (window.GV.refreshAiSuggest) window.GV.refreshAiSuggest(true);
    });
    $('crashModalClose')?.addEventListener('click', () => $('crashModal').close());
    document.querySelectorAll('input[name="crashScope"]').forEach((el) => {
      el.addEventListener('change', refreshIssues);
    });
    $('layerGps')?.addEventListener('change', () => {
      gpsLine.setLatLngs(gpsOn() ? lastGpsPoints : []);
      applyLayers();
    });
    $('layerImu')?.addEventListener('change', () => {
      imuLine.setLatLngs(imuOn() ? lastImuPoints : []);
      applyLayers();
    });
    $('clearRouteBtn')?.addEventListener('click', async () => {
      const c = loadCreds();
      if (!c.device) return;
      await apiSend('DELETE', '/geo/' + encodeURIComponent(c.device) + '/route');
      gpsLine.setLatLngs([]); imuLine.setLatLngs([]); osrmLine.setLatLngs([]);
      lastGpsPoints = []; lastImuPoints = []; lastOsrmKey = ''; framedOnce = false;
    });
    $('deviceId')?.addEventListener('change', () => {
      saveCreds({ device: $('deviceId').value });
      setChip();
      if (window.GV.refreshAiSuggest) window.GV.refreshAiSuggest(true);
    });
    $('insDeviceSelect')?.addEventListener('change', () => {
      const id = $('insDeviceSelect').value;
      saveCreds({ device: id });
      const hub = $('deviceId');
      if (hub) hub.value = id;
      setChip();
      refreshInsights();
    });
  });
})();
