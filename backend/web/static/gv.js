/* Good Vibes dashboard shell — cookies, tabs, API, board refresh. */
(function (global) {
  const API = '/app/good_vibes/v1';
  const BASE = '/app/good_vibes';
  const POLL_MS = 10000;
  const COOKIE = { key: 'gv_api_key', group: 'gv_group_id', device: 'gv_device_id' };
  const TABS = ['hub', 'body', 'vibro', 'ahrs', 'insights', 'issues'];
  const PROTECTED = new Set(['body', 'vibro', 'ahrs', 'insights', 'issues']);
  const ALIAS = { wearable: 'body', map: 'ahrs', '': 'hub', index: 'hub' };
  const GBASE = '/app/good_vibes/grafana/';
  const GRAFANA = {
    hub: { href: GBASE, label: 'Grafana' },
    body: { href: GBASE + 'd/imu-wearable/wearable-mt200-2b-imu-walk-2b-rssi', label: 'Wearable dashboard' },
    vibro: { href: GBASE + 'd/imu-verdicts/esp32-imu-verdicts', label: 'ESP32 IMU Verdicts' },
    ahrs: { href: GBASE + 'd/imu-verdicts/esp32-imu-verdicts', label: 'ESP32 IMU Verdicts' },
    insights: { href: GBASE + 'd/imu-battery-bench/esp32-battery-bench', label: 'Battery bench' },
    issues: { href: GBASE + 'd/imu-crashes/esp32-imu-crashes', label: 'ESP32 IMU Crashes' },
  };
  const SUBTITLE = {
    hub: 'Demo access, connection, export, and device config',
    body: 'MT200 body sensor — HR, SpO2, steps, kcal, distance, walk, hop RSSI',
    vibro: 'Machine / operator verdicts and recent candidates',
    ahrs: 'Live orientation cube and GPS + IMU route',
    insights: 'Device metrics: battery, uptime, RSSI, OTA',
    issues: 'Crash reports and firmware OTA outcomes',
  };

  const $ = (id) => document.getElementById(id);
  const DEMO = (() => {
    try {
      return JSON.parse((document.getElementById('demo-hints') || {}).textContent || '{}');
    } catch {
      return {};
    }
  })();

  function cookieGet(name) {
    const m = document.cookie.match(new RegExp('(?:^|; )' + name.replace(/[.$?*|{}()[\]\\/+^]/g, '\\$&') + '=([^;]*)'));
    return m ? decodeURIComponent(m[1]) : '';
  }
  function cookieSet(name, value) {
    document.cookie = name + '=' + encodeURIComponent(value || '') +
      '; path=/app/good_vibes; max-age=31536000; SameSite=Lax';
  }

  function loadCreds() {
    let key = cookieGet(COOKIE.key) || localStorage.getItem('gv_api_key') || DEMO.api_key || '';
    let group = cookieGet(COOKIE.group) || localStorage.getItem('gv_group_id') || 'default';
    let device = cookieGet(COOKIE.device) || localStorage.getItem('gv_device_id') || '';
    if (key && !cookieGet(COOKIE.key)) cookieSet(COOKIE.key, key);
    if (group && !cookieGet(COOKIE.group)) cookieSet(COOKIE.group, group);
    if (device && !cookieGet(COOKIE.device)) cookieSet(COOKIE.device, device);
    return { key, group, device };
  }
  function saveCreds(partial) {
    const cur = loadCreds();
    const next = {
      key: partial.key != null ? partial.key : cur.key,
      group: partial.group != null ? partial.group : cur.group,
      device: partial.device != null ? partial.device : cur.device,
    };
    cookieSet(COOKIE.key, next.key);
    cookieSet(COOKIE.group, next.group);
    cookieSet(COOKIE.device, next.device);
    try {
      localStorage.setItem('gv_api_key', next.key);
      localStorage.setItem('gv_group_id', next.group);
      localStorage.setItem('gv_device_id', next.device);
    } catch { /* private mode */ }
    return next;
  }
  function credsComplete() {
    const c = loadCreds();
    return !!(c.key && c.group && c.device);
  }

  async function apiGet(path) {
    const key = loadCreds().key;
    if (!key) throw new Error('API key required');
    const res = await fetch(API + path, { headers: { 'X-API-Key': key } });
    if (!res.ok) throw new Error(await res.text());
    return res.json();
  }
  async function apiSend(method, path, body) {
    const key = loadCreds().key;
    if (!key) throw new Error('API key required');
    const res = await fetch(API + path, {
      method,
      headers: { 'X-API-Key': key, 'Content-Type': 'application/json' },
      body: body != null ? JSON.stringify(body) : undefined,
    });
    if (!res.ok) throw new Error(await res.text());
    if (res.status === 204) return null;
    return res.json();
  }
  const apiPost = (path, body) => apiSend('POST', path, body);
  const apiPut = (path, body) => apiSend('PUT', path, body);

  function fmtTs(ms) { return new Date(ms).toLocaleString(); }
  function fmtAge(ms) {
    if (ms == null || ms < 0) return '—';
    let s = Math.max(0, Math.round(ms / 1000));
    if (s < 60) return s + 's';
    const pad = (n) => String(n).padStart(2, '0');
    const days = Math.floor(s / 86400);
    s %= 86400;
    const h = Math.floor(s / 3600);
    s %= 3600;
    const m = Math.floor(s / 60);
    const sec = s % 60;
    if (days > 0) return `${days}d ${h}:${pad(m)}:${pad(sec)}`;
    if (h > 0) return `${h}:${pad(m)}:${pad(sec)}`;
    return `${m}:${pad(sec)}`;
  }
  function fmtPc(pc) {
    if (pc == null) return '—';
    return '0x' + pc.toString(16).padStart(8, '0');
  }
  function fmtExcCause(c, reason) {
    if (c == null) return '—';
    const wdt = typeof reason === 'string' && /wdt|watchdog/i.test(reason);
    if (c === 0 && wdt) return 'none (watchdog stall)';
    if (c === 0 && (reason === 'task_wdt' || reason === 'wdt' || reason === 'int_wdt')) {
      return 'none (watchdog stall)';
    }
    const names = {
      0: 'IllegalInstruction', 1: 'Syscall', 2: 'InstrFetchError', 3: 'LoadStoreError',
      4: 'Level1Interrupt', 5: 'Alloca', 6: 'IntegerDivideByZero', 7: 'PCValue',
      8: 'Privileged', 9: 'LoadStoreAlignment', 12: 'InstrPIFData', 13: 'LoadStorePIF',
      14: 'InstrFetchProhibited', 15: 'LoadStoreProhibited',
    };
    return names[c] ? `${names[c]} (${c})` : String(c);
  }
  function levelClass(l) { return 'lvl-' + Math.min(2, Math.max(0, l)); }
  function levelName(l) { return ['OK', 'WARN', 'ALERT'][l] || '?'; }
  function candidateName(l) { return ['cand OK', 'cand WARN', 'cand ALERT'][l] || '?'; }
  function operatorName(l) { return ['OK', 'WARN', 'ALARM'][l] || '?'; }

  function tabFromPath() {
    const raw = (location.pathname.replace(/\/+$/, '').split('/').pop() || 'hub');
    return ALIAS[raw] || (TABS.includes(raw) ? raw : 'hub');
  }
  function pathFor(tab) {
    return tab === 'hub' ? BASE + '/' : BASE + '/' + tab;
  }

  const onShow = {};
  let currentTab = 'hub';
  let pollTimer = null;
  let liveTimer = null;
  let pendingNeed = false;
  let pendingNext = '';

  function setGrafana(tab) {
    const g = GRAFANA[tab];
    const el = $('pageGrafanaLink');
    if (!el || !g) return;
    el.href = g.href;
    el.textContent = g.label;
    el.style.display = g.href ? '' : 'none';
  }
  function setChip() {
    const c = loadCreds();
    const el = $('tabDeviceChip');
    if (!el) return;
    el.textContent = c.device ? (c.group + ' · ' + c.device) : 'no device';
  }
  function markRequired(on) {
    ['lblApiKey', 'lblGroupId', 'lblDeviceId'].forEach((id) => {
      const el = $(id);
      if (el) el.classList.toggle('required', on);
    });
    const banner = $('needBanner');
    if (banner) banner.classList.toggle('visible', on);
  }
  function showBoard(tab, replace) {
    if (PROTECTED.has(tab) && !credsComplete()) {
      pendingNeed = true;
      pendingNext = tab;
      tab = 'hub';
      const url = pathFor('hub') + '?need=1&next=' + encodeURIComponent(pendingNext);
      history[replace ? 'replaceState' : 'pushState']({ tab: 'hub' }, '', url);
    } else if (!replace && tab !== currentTab) {
      history.pushState({ tab }, '', pathFor(tab));
    }
    currentTab = tab;
    document.querySelectorAll('.board').forEach((b) => {
      b.classList.toggle('active', b.dataset.board === tab);
    });
    document.querySelectorAll('nav.tabbar a.tab').forEach((a) => {
      a.classList.toggle('active', a.dataset.tab === tab);
    });
    const sub = $('pageSubtitle');
    if (sub) sub.textContent = SUBTITLE[tab] || '';
    setGrafana(tab);
    setChip();
    markRequired(pendingNeed && tab === 'hub');
    if (onShow[tab]) onShow[tab]();
    restartPoll();
    restartAiPoll();
  }
  function restartPoll() {
    if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
    if (liveTimer) { clearInterval(liveTimer); liveTimer = null; }
    pollTimer = setInterval(() => {
      if (onShow[currentTab]) onShow[currentTab]();
    }, POLL_MS);
    if (currentTab === 'ahrs' && onShow.ahrs) {
      liveTimer = setInterval(() => onShow.ahrs(), 2000);
    }
  }
  function go(tab) {
    pendingNeed = false;
    showBoard(tab, false);
  }

  /* ---------- AI suggests (Body / Vibro) ---------- */
  const AI_BOARDS = new Set(['body', 'vibro']);
  const AI_POLL_MS = 60000;
  let aiTimer = null;
  let aiAbort = null;
  let aiSeq = 0;
  let aiLastKey = '';

  function aiTzOffsetMin() {
    return -new Date().getTimezoneOffset();
  }
  function aiSetVisible(on) {
    const panel = $('aiSuggestPanel');
    if (panel) panel.hidden = !on;
  }
  function aiSetLoading(on) {
    const loader = $('aiSuggestLoader');
    if (loader) loader.hidden = !on;
  }
  function aiSetText(text, meta) {
    const el = $('aiSuggestText');
    if (el) el.textContent = text || '';
    const m = $('aiSuggestMeta');
    if (m) m.textContent = meta || '';
  }
  function aiContextKey(tab) {
    const c = loadCreds();
    const mk = (tab === 'vibro' && $('machineKey') && $('machineKey').value) || '';
    return [tab, c.device || '', mk].join('|');
  }
  async function refreshAiSuggest(force) {
    const tab = currentTab;
    if (!AI_BOARDS.has(tab) || !credsComplete()) {
      aiSetVisible(false);
      if (aiAbort) { try { aiAbort.abort(); } catch { /* */ } aiAbort = null; }
      return;
    }
    aiSetVisible(true);
    const key = aiContextKey(tab);
    if (!force && key === aiLastKey) return;
    const seq = ++aiSeq;
    if (aiAbort) { try { aiAbort.abort(); } catch { /* */ } }
    aiAbort = new AbortController();
    aiSetLoading(true);
    if (key !== aiLastKey) aiSetText('', '');
    const c = loadCreds();
    const params = new URLSearchParams({
      board: tab,
      device_id: c.device,
      tz_offset_min: String(aiTzOffsetMin()),
    });
    if (tab === 'vibro') {
      const mk = $('machineKey') && $('machineKey').value;
      if (mk) params.set('machine_key', mk);
    }
    try {
      const res = await fetch(API + '/ai/suggest?' + params.toString(), {
        headers: { 'X-API-Key': c.key },
        signal: aiAbort.signal,
      });
      if (!res.ok) throw new Error(await res.text());
      const data = await res.json();
      if (seq !== aiSeq) return;
      aiLastKey = key;
      const meta = data.model ? data.model.replace(/:latest$/, '') : '';
      aiSetText(data.suggestion || '—', meta);
    } catch (e) {
      if (e && e.name === 'AbortError') return;
      if (seq !== aiSeq) return;
      aiSetText('AI temporarily unavailable — will retry.', '');
    } finally {
      if (seq === aiSeq) aiSetLoading(false);
    }
  }
  function restartAiPoll() {
    if (aiTimer) { clearInterval(aiTimer); aiTimer = null; }
    if (!AI_BOARDS.has(currentTab)) {
      aiSetVisible(false);
      return;
    }
    refreshAiSuggest(true);
    aiTimer = setInterval(() => refreshAiSuggest(true), AI_POLL_MS);
  }

  function bindHubForm() {
    const c = loadCreds();
    if ($('apiKey')) $('apiKey').value = c.key;
    if ($('groupId')) $('groupId').value = c.group || 'default';
    if ($('deviceId') && c.device) $('deviceId').dataset.pending = c.device;
    const params = new URLSearchParams(location.search);
    if (params.get('need') === '1') {
      pendingNeed = true;
      pendingNext = params.get('next') || '';
    }
  }
  function readHubForm() {
    return saveCreds({
      key: ($('apiKey') && $('apiKey').value.trim()) || '',
      group: ($('groupId') && $('groupId').value.trim()) || 'default',
      device: ($('deviceId') && $('deviceId').value) || '',
    });
  }

  function applyDemoHints() {
    const key = DEMO.api_key || '';
    if (key && $('apiKeyHint')) $('apiKeyHint').textContent = 'X-API-Key: ' + key;
    if (DEMO.grafana_user && $('grafanaUserHint')) $('grafanaUserHint').textContent = DEMO.grafana_user;
    if ($('grafanaPassHint')) {
      $('grafanaPassHint').textContent = DEMO.grafana_password || '(see server .grafana_admin_password)';
    }
      if ($('grafanaHomeLink')) $('grafanaHomeLink').href = '/app/good_vibes/grafana/';
  }

  function phoneSetupLink() {
    const c = loadCreds();
    if (!c.key) throw new Error('API key required');
    const params = new URLSearchParams({ u: location.origin + BASE, k: c.key, g: c.group || 'default' });
    if (c.device) params.set('d', c.device);
    return 'goodvibes://cloud?' + params.toString();
  }

  global.GV = {
    $, API, DEMO, POLL_MS,
    loadCreds, saveCreds, credsComplete, readHubForm,
    apiGet, apiPost, apiPut, apiSend,
    fmtTs, fmtAge, fmtPc, fmtExcCause,
    levelClass, levelName, candidateName, operatorName,
    go, showBoard, onShow, setChip,
    phoneSetupLink,
    refreshAiSuggest,
    current: () => currentTab,
  };

  document.addEventListener('DOMContentLoaded', () => {
    applyDemoHints();
    bindHubForm();
    document.querySelectorAll('nav.tabbar a.tab').forEach((a) => {
      a.addEventListener('click', (e) => {
        e.preventDefault();
        go(a.dataset.tab);
      });
    });
    window.addEventListener('popstate', () => showBoard(tabFromPath(), true));
    const start = tabFromPath();
    showBoard(start, true);
    if (pendingNeed && pendingNext && credsComplete()) {
      const next = pendingNext;
      pendingNeed = false;
      pendingNext = '';
      go(next);
    }
  });
})(window);
