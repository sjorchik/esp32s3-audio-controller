'use strict';
/* Сторінка «Головна» (Prompt 19). Залежить від common.js і nav.js. */
(() => {
  const STREAM = {
    Idle: ['Зупинено', ''], Connecting: ['Підключення…', 'badge-warn'],
    Buffering: ['Буферизація…', 'badge-warn'], Playing: ['Грає', 'badge-ok'],
    Error: ['Помилка потоку', 'badge-err'], Reconnecting: ['Перепідключення…', 'badge-warn'],
  };
  const STREAM_ACTIVE = new Set(['Connecting', 'Buffering', 'Playing', 'Reconnecting']);

  const g = (id) => document.getElementById(id);
  const ui = {
    wifi: g('wifi'), power: g('btn-power'), wake: g('btn-wake'), standby: g('standby'), live: g('live'),
    grpPlayer: g('grp-player'), grpVolume: g('grp-volume'), grpInput: g('grp-input'), grpStations: g('grp-stations'),
    badge: g('badge'), inputName: g('input-name'), name: g('station-name'), track: g('track'),
    dial: g('dial'), needle: g('dial-needle'), dialCaption: g('dial-caption'), hint: g('player-hint'),
    prev: g('btn-prev'), play: g('btn-play'), next: g('btn-next'),
    vol: g('vol'), volOut: g('vol-out'), volDn: g('vol-dn'), volUp: g('vol-up'), mute: g('btn-mute'),
    inputs: g('inputs'),
    stCount: g('st-count'), stRefresh: g('st-refresh'), stSearch: g('st-search'), stList: g('st-list'), stEmpty: g('st-empty'),
  };

  let st = null;                 // останній /api/status
  let stations = [];             // останній /api/stations
  let rows = [];                 // [{li, btn, key, index}]
  let rowByIndex = new Map();
  let currentRow = null;
  let stationsLoaded = false;
  let stationsBusy = false;
  let stationsNextTry = 0;
  let countTried = -1;
  let scrolledOnce = false;
  let inputsSig = '';

  /* ---------- Стан ---------- */
  const isStandby = () => !!st && (st.standby === true || st.mode === 'Standby');
  const isRadio = () => !!st && st.input === 0;
  const caps = () => (st && st.processor && st.processor.ready ? st.processor.capabilities : null);

  function lockKind() {
    if (!st) return 'loading';
    if (connection.offline) return 'offline';
    if (st.mode === 'OtaUpdate') return 'ota';
    if (st.mode === 'IrLearn') return 'irlearn';
    if (st.mode === 'WifiSetup') return 'wifisetup';
    return null;
  }

  /* ---------- Біжучий рядок ---------- */
  function measureMarquee(node) {
    const inner = node.firstChild;
    node.classList.remove('is-scrolling');
    if (node.clientWidth === 0) return;   // прихований (standby): перемірюємо після показу
    const over = inner.offsetWidth - node.clientWidth;
    if (over > 2) {
      node.style.setProperty('--shift', -over + 'px');
      node.style.setProperty('--dur', (over / CONFIG.marqueePxPerSec + 2 * CONFIG.marqueePauseS).toFixed(1) + 's');
      node.classList.add('is-scrolling');
    }
  }
  function setMarquee(node, text) {
    if (node.dataset.t === text) return;
    node.dataset.t = text;
    node.firstChild.textContent = text || '\u00a0';
    node.classList.remove('is-scrolling');
    requestAnimationFrame(() => measureMarquee(node));
  }
  let resizeTimer = null;
  window.addEventListener('resize', () => {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(() => { measureMarquee(ui.name); measureMarquee(ui.track); }, CONFIG.resizeDebounceMs);
  });

  /* ---------- Рендер ---------- */
  function renderInputs() {
    const list = st.inputs || [];
    const sig = JSON.stringify(list.map((i) => [i.index, i.name, i.available]));
    if (sig !== inputsSig) {
      inputsSig = sig;
      ui.inputs.replaceChildren(...list.filter((i) => i.available).map((i) =>
        el('button', { type: 'button', role: 'radio', 'data-i': i.index }, i.name)));
    }
    [...ui.inputs.children].forEach((b) => setAttr(b, 'aria-checked', Number(b.dataset.i) === st.input));
  }

  function renderBanners(lock) {
    setBanner('ota', lock === 'ota' ? 'Оновлення прошивки: ' + (st.otaProgress || 0) + '%. Керування вимкнено.' : null,
      'warn', { progress: st && st.otaProgress });
    setBanner('irlearn', lock === 'irlearn' ? 'Іде навчання кнопок пульта. Керування вимкнено.' : null, 'warn');
    setBanner('wifisetup', lock === 'wifisetup' ? 'Пристрій у режимі налаштування Wi-Fi. Керування вимкнено.' : null, 'warn');
  }

  function renderStreamBadge() {
    const [label, cls] = isRadio() ? (STREAM[st.streamStatus] || [st.streamStatus, '']) : ['Зовнішній вхід', ''];
    setText(ui.badge, label);
    const c = 'badge' + (cls ? ' ' + cls : '');
    if (ui.badge.className !== c) ui.badge.className = c;
  }

  function render() {
    if (!st) {
      [ui.grpPlayer, ui.grpVolume, ui.grpInput, ui.grpStations, ui.power].forEach((n) => { n.disabled = true; });
      return;
    }
    const lock = lockKind();
    const standby = isStandby();
    const radio = isRadio();
    const c = caps();
    const locked = lock !== null;

    renderBanners(lock);
    renderWifi(ui.wifi, st.wifi);
    const wasHidden = ui.live.hidden;
    ui.standby.hidden = !standby;
    ui.live.hidden = standby;
    if (wasHidden && !standby) requestAnimationFrame(() => { measureMarquee(ui.name); measureMarquee(ui.track); });
    ui.power.disabled = lock === 'ota' || lock === 'irlearn' || lock === 'offline' || lock === 'loading';
    setAttr(ui.power, 'aria-pressed', !standby);
    ui.wake.disabled = ui.power.disabled;

    ui.grpPlayer.disabled = locked || !radio;
    ui.grpStations.disabled = locked || !radio;
    ui.grpVolume.disabled = locked || !c;
    ui.grpInput.disabled = locked || !c;

    // --- Плеєр ---
    renderStreamBadge();
    setText(ui.inputName, st.inputName || '');
    const sn = st.station || {};
    const listed = radio && stations[sn.index];   // назва зі списку має пріоритет над ICY-назвою потоку
    setMarquee(ui.name, radio ? ((listed && listed.name) || sn.name || '—') : (st.inputName || '—'));
    setMarquee(ui.track, radio ? (st.track || '') : '');
    const inputs = st.inputs || [];
    ui.hint.hidden = radio;
    if (!radio) setText(ui.hint, 'Плеєр і список станцій доступні лише на вході «' + ((inputs[0] && inputs[0].name) || 'WiFi Radio') + '».');
    const active = radio && STREAM_ACTIVE.has(st.streamStatus);
    setIcon(ui.play, active ? 'pause' : 'play');
    setAttr(ui.play, 'aria-label', active ? 'Пауза' : 'Грати');
    const count = sn.count || 0;
    ui.dial.hidden = !(radio && count > 0);
    if (!ui.dial.hidden) {
      const idx = clamp(sn.index || 0, 0, count - 1);
      ui.needle.style.left = (count > 1 ? (idx / (count - 1)) * 100 : 50) + '%';
      setText(ui.dialCaption, 'Станція ' + (idx + 1) + ' із ' + count);
    }

    // --- Гучність ---
    if (c) {
      setAttr(ui.vol, 'min', c.volumeMin);
      setAttr(ui.vol, 'max', c.volumeMax);
      if (!volHeld()) { ui.vol.value = st.volume; setText(ui.volOut, String(st.volume)); }
    }
    setAttr(ui.mute, 'aria-pressed', !!st.mute);
    setAttr(ui.mute, 'aria-label', st.mute ? 'Увімкнути звук' : 'Вимкнути звук');
    setIcon(ui.mute, st.mute ? 'mute' : 'volume');

    renderInputs();
    renderCurrentStation();
  }

  /* ---------- Команди ---------- */
  async function command(path, body) {
    try {
      const r = await api.post(path, body);
      if (st && r.state) {
        st.mode = r.state.mode; st.standby = r.state.standby; st.input = r.state.input; st.mute = r.state.mute;
        if (!volHeld()) st.volume = r.state.volume;
      }
      render();
      poller.now();
      return r;
    } catch (e) {
      toast(errorText(e), 'error');
      render();
      poller.now();
      return null;
    }
  }

  /* ---------- Гучність (оптимістичний UI, тротлінг) ---------- */
  const vol = { dragging: false, holdUntil: 0, pending: null, inflight: false, lastSent: 0, timer: null };
  function volHeld() { return vol.dragging || Date.now() < vol.holdUntil; }
  function rollbackVolume() {
    vol.holdUntil = 0; vol.pending = null;
    if (st) { ui.vol.value = st.volume; setText(ui.volOut, String(st.volume)); }
  }
  function queueVolume(v, final) {
    vol.pending = v;
    if (final) { clearTimeout(vol.timer); vol.timer = null; flushVolume(); return; }
    if (vol.timer || vol.inflight) return;
    const wait = Math.max(0, CONFIG.volumeSendMinMs - (Date.now() - vol.lastSent));
    vol.timer = setTimeout(() => { vol.timer = null; flushVolume(); }, wait);
  }
  async function flushVolume() {
    if (vol.inflight || vol.pending === null) return;
    const v = vol.pending;
    vol.pending = null; vol.inflight = true; vol.lastSent = Date.now();
    try {
      const r = await api.post('/api/volume', { value: v });
      if (st && r.state) st.volume = r.state.volume;
    } catch (e) {
      toast(errorText(e), 'error');
      rollbackVolume();
    }
    vol.inflight = false;
    if (vol.pending !== null) queueVolume(vol.pending, false);
  }
  ui.vol.addEventListener('pointerdown', () => { vol.dragging = true; });
  ['pointerup', 'pointercancel'].forEach((ev) => window.addEventListener(ev, () => {
    if (vol.dragging) { vol.dragging = false; vol.holdUntil = Date.now() + CONFIG.volumeHoldMs; }
  }));
  ui.vol.addEventListener('input', () => {
    const v = Number(ui.vol.value);
    setText(ui.volOut, String(v));
    vol.holdUntil = Date.now() + CONFIG.volumeHoldMs;
    queueVolume(v, false);
  });
  ui.vol.addEventListener('change', () => {
    vol.holdUntil = Date.now() + CONFIG.volumeHoldMs;
    queueVolume(Number(ui.vol.value), true);
  });
  async function volumeStep(sign) {
    const r = await command('/api/volume', { step: sign * CONFIG.volumeButtonStep });
    if (r && r.state) { ui.vol.value = r.state.volume; setText(ui.volOut, String(r.state.volume)); }
  }
  ui.volDn.addEventListener('click', () => volumeStep(-1));
  ui.volUp.addEventListener('click', () => volumeStep(1));
  ui.mute.addEventListener('click', async () => {
    if (!st) return;
    const want = !st.mute;
    st.mute = want; render();
    if (!(await command('/api/mute', { mute: want }))) { st.mute = !want; render(); }
  });

  /* ---------- Плеєр, живлення, вхід ---------- */
  ui.play.addEventListener('click', () => {
    const active = st && STREAM_ACTIVE.has(st.streamStatus);
    command(active ? '/api/player/pause' : '/api/player/play');
  });
  ui.prev.addEventListener('click', () => command('/api/player/prev'));
  ui.next.addEventListener('click', () => command('/api/player/next'));
  ui.power.addEventListener('click', () => command('/api/power', { state: 'toggle' }));
  ui.wake.addEventListener('click', () => command('/api/power', { state: 'on' }));
  ui.inputs.addEventListener('click', (ev) => {
    const b = ev.target.closest('button[data-i]');
    if (!b || !st) return;
    const i = Number(b.dataset.i);
    if (i === st.input) return;
    st.input = i; render();
    command('/api/input', { index: i });
  });

  /* ---------- Станції ---------- */
  function buildStationList() {
    const frag = document.createDocumentFragment();
    rows = []; rowByIndex = new Map(); currentRow = null;
    stations.forEach((s, pos) => {
      const index = typeof s.index === 'number' ? s.index : pos;
      const btn = el('button', { type: 'button', class: 'row', 'data-i': index },
        el('span', { class: 'idx' }, index + 1), el('span', { class: 'name' }, s.name || ''));
      const li = el('li', null, btn);
      const row = { li, btn, index, key: String(s.name || '').toLowerCase() };
      rows.push(row); rowByIndex.set(index, row);
      frag.append(li);
    });
    ui.stList.replaceChildren(frag);
    setText(ui.stCount, stations.length ? '(' + stations.length + ')' : '');
    ui.stSearch.hidden = stations.length <= CONFIG.stationSearchThreshold;
    ui.stSearch.value = '';
    updateStationsEmpty(stations.length === 0);
    renderCurrentStation();
  }

  function updateStationsEmpty(empty) {
    ui.stEmpty.hidden = !empty;
    if (!empty) return;
    const p = pageById('stations');
    ui.stEmpty.replaceChildren(...(p && p.enabled
      ? ['Список порожній. Додайте станції на сторінці «', el('a', { href: p.href }, p.title), '».']
      : ['Список порожній. Станції можна буде додати на сторінці «Станції» (скоро).']));
  }

  function renderCurrentStation() {
    if (!st || !rows.length) return;
    const row = isRadio() ? rowByIndex.get((st.station || {}).index) || null : null;
    if (row !== currentRow) {
      if (currentRow) currentRow.btn.removeAttribute('aria-current');
      if (row) row.btn.setAttribute('aria-current', 'true');
      currentRow = row;
    }
    if (row && !scrolledOnce && !ui.stList.closest('[hidden]')) {
      scrolledOnce = true;
      requestAnimationFrame(() => {
        ui.stList.scrollTop = row.li.offsetTop - (ui.stList.clientHeight - row.li.offsetHeight) / 2;
      });
    }
  }

  async function loadStations(manual) {
    if (stationsBusy) return;
    stationsBusy = true;
    ui.stRefresh.setAttribute('aria-busy', 'true');
    try {
      const list = await api.get('/api/stations', CONFIG.stationsTimeoutMs);
      stations = Array.isArray(list) ? list : [];
      stationsLoaded = true;
      buildStationList();
      if (manual) toast('Список станцій оновлено', 'ok');
    } catch (e) {
      stationsNextTry = Date.now() + CONFIG.stationsRetryMs;
      if (manual || !stationsLoaded) toast('Список станцій: ' + errorText(e), 'error');
    }
    ui.stRefresh.removeAttribute('aria-busy');
    stationsBusy = false;
  }

  function maybeLoadStations() {
    if (stationsBusy || Date.now() < stationsNextTry) return;
    const count = st.station ? st.station.count : 0;
    if (!stationsLoaded) { loadStations(false); return; }
    if (count !== stations.length && count !== countTried) { countTried = count; loadStations(false); }
  }

  ui.stRefresh.addEventListener('click', () => loadStations(true));
  ui.stSearch.addEventListener('input', () => {
    const q = ui.stSearch.value.trim().toLowerCase();
    let shown = 0;
    rows.forEach((r) => { const hide = q !== '' && !r.key.includes(q); r.li.hidden = hide; if (!hide) shown++; });
    ui.stEmpty.hidden = shown > 0 || stations.length === 0;
    if (!ui.stEmpty.hidden) ui.stEmpty.textContent = 'Нічого не знайдено.';
  });
  ui.stList.addEventListener('click', (ev) => {
    const b = ev.target.closest('button.row');
    if (!b || !st) return;
    const i = Number(b.dataset.i);
    if (st.station) st.station.index = i;
    renderCurrentStation();
    command('/api/player/station', { index: i });
  });

  /* ---------- Опитування ---------- */
  async function refreshStatus() {
    st = await api.get('/api/status');
    render();
    maybeLoadStations();
  }
  const poller = createPoller(refreshStatus, { intervalMs: CONFIG.pollStatusMs });

  initShell('home');
  onConnectionChange(render);
  render();
  poller.start();
})();
