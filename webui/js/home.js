'use strict';
/* Сторінка «Головна» (Prompt 19; P51a: без аудіочіпа / тембру; USB16a-1: 4 входи, слот 3 = USB із заглушкою замість
   блоків радіо й станцій). Залежить від common.js і nav.js. */
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
    grpUsb: g('grp-usb'), usbName: g('usb-name'),
    badge: g('badge'), inputName: g('input-name'), menuNote: g('menu-note'), name: g('station-name'), track: g('track'),
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
  let lastUsb = false;           // попередній render був у режимі USB (для перемірювання біжучих рядків)
  const pending = createInputPending();   // очікування 202 pending (вхід у USB / вихід з нього)
  let pendTimer = null;

  /* ---------- Стан ---------- */
  const isStandby = () => !!st && (st.standby === true || st.mode === 'Standby');
  const isRadio = () => !!st && st.input === 0;
  const isUsb = () => !!st && st.input === CONFIG.usbInputIndex;   // INP4b: status.input = 3, поки активний режим USB
  /* Що відкрито на екрані пристрою (P50a: status.menuContext при mode === 'Menu'). Невідоме значення = «меню». */
  const MENU_TEXT = {
    StationList: 'На пристрої відкрито список станцій.',
    EqBalance: 'На пристрої відкрито екран «EQ + баланс».',
  };
  const menuText = () => (st && st.mode === 'Menu' ? MENU_TEXT[st.menuContext] || 'На пристрої відкрито меню.' : '');

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
    /* Назви й доступність — з GET /api/inputs (кеш common.js); до першої відповіді — запасно з status.inputs.
       Недоступні й вимкнені входи ховаємо (P39b) — так само, як вони зникають із перемикання на пристрої. */
    const d = getInputs();
    const src = d ? d.inputs.filter((i) => i.selectable !== false && i.available !== false && i.enabled !== false)
                  : (st.inputs || []).filter((i) => i.available && isInputEnabled(i.index));
    const list = src.slice(0, CONFIG.inputSlotCount);
    const sig = JSON.stringify(list.map((i) => [i.index, i.name]));
    if (sig !== inputsSig) {
      inputsSig = sig;
      ui.inputs.replaceChildren(...list.map((i) =>
        el('button', { type: 'button', role: 'radio', 'data-i': i.index }, i.name)));
    }
    const pd = pending.get();
    [...ui.inputs.children].forEach((b) => {
      const i = Number(b.dataset.i);
      const entry = list.find((x) => x.index === i);
      const busy = !!pd && pd.target === i;
      // «Підключення…» / «Перемикання…» на цільовій кнопці до status.input === ціль (st.input НЕ чіпаємо)
      setText(b, busy ? (i === CONFIG.usbInputIndex ? 'Підключення…' : 'Перемикання…') : ((entry && entry.name) || ''));
      setAttr(b, 'aria-busy', busy);
      setAttr(b, 'aria-checked', i === st.input);
    });
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
    const usb = isUsb();
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

    // USB (INP4b): блоки радіо й станцій ховаємо, на місці станцій — тимчасова заглушка (USB16b замінить її плеєром).
    // status.mode / station / playing у цей час належать ПОПЕРЕДНЬОМУ входу й не використовуються.
    ui.grpPlayer.hidden = usb;
    ui.grpStations.hidden = usb;
    ui.grpUsb.hidden = !usb;
    if (usb) setText(ui.usbName, st.inputName || inputName(CONFIG.usbInputIndex, st.inputs));
    if (lastUsb && !usb) requestAnimationFrame(() => { measureMarquee(ui.name); measureMarquee(ui.track); });
    lastUsb = usb;
    ui.grpPlayer.disabled = locked || !radio;
    ui.grpStations.disabled = locked || !radio;
    ui.grpVolume.disabled = locked;
    ui.grpInput.disabled = locked;

    // --- Плеєр ---
    renderStreamBadge();
    setText(ui.inputName, st.inputName || '');
    const mt = menuText();                       // позначка про меню на пристрої; керування не блокує
    ui.menuNote.hidden = !mt;
    setText(ui.menuNote, mt);
    const sn = st.station || {};
    setMarquee(ui.name, radio ? (sn.name || '—') : (st.inputName || '—'));  // station.name = назва зі списку (P20b)
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
    setAttr(ui.vol, 'min', CONFIG.volumeMin);      // min / max — ПЕРЕД value
    setAttr(ui.vol, 'max', CONFIG.volumeMax);
    if (!volHeld()) { ui.vol.value = st.volume; setText(ui.volOut, String(st.volume)); }
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
      noteInputError(e);
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
  /* Вибір входу. Крок, що торкається USB (вхід у режим або вихід із нього) або йде під час очікування, дає 202 pending
     РАНІШЕ, ніж status.input зміниться: st.input НЕ ставимо, а показуємо «Підключення…» на цільовій кнопці до збігу
     status.input з ціллю або таймауту CONFIG.inputPendingTimeoutMs. Решта входів — як раніше (оптимістично). */
  function schedulePendingPoll() {
    clearTimeout(pendTimer);
    pendTimer = setTimeout(() => {
      pendTimer = null;
      if (!pending.get()) return;
      poller.now();
      schedulePendingPoll();
    }, CONFIG.inputPendingPollMs);
  }
  async function selectInput(i) {
    const pd = pending.get();
    if (i === st.input && !pd) return;
    const usbStep = !!pd || i === CONFIG.usbInputIndex || st.input === CONFIG.usbInputIndex;
    if (!usbStep) {
      st.input = i; render();
      command('/api/input', { index: i });
      return;
    }
    pending.start(i, i === CONFIG.usbInputIndex ? 'usb_enter' : 'usb_exit');
    render();
    schedulePendingPoll();
    const r = await command('/api/input', { index: i });
    const p = r ? pendingFromResponse(r) : null;
    if (!p) pending.clear();                                       // відмова (409 …) або 200 без pending
    else if (pending.get()) pending.start(p.target, p.reason);     // сервер підтвердив ціль; poller міг уже завершити очікування
    render();
  }
  ui.inputs.addEventListener('click', (ev) => {
    const b = ev.target.closest('button[data-i]');
    if (!b || !st) return;
    selectInput(Number(b.dataset.i));
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
    if (isStandby()) pending.clear();            // standby виводить із режиму USB: очікувати нічого
    const pd = pending.get();
    if (pending.check(st.input) === 'timeout') {
      toast(pd && pd.reason === 'usb_exit'
        ? 'Пристрій не перемкнувся на вибраний вхід вчасно. Спробуйте ще раз.'
        : 'USB-вхід не підключився вчасно. Спробуйте ще раз.', 'error');
    }
    syncInputs(st.input);
    render();
    maybeLoadStations();
  }
  const poller = createPoller(refreshStatus, { intervalMs: CONFIG.pollStatusMs });

  initShell('home');
  onConnectionChange(render);
  onInputsChange(() => { if (st) render(); });
  render();
  poller.start();
})();


